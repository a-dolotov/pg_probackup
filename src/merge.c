/*-------------------------------------------------------------------------
 *
 * merge.c: merge FULL and incremental backups
 *
 * Copyright (c) 2018, Postgres Professional
 *
 *-------------------------------------------------------------------------
 */

#include "pg_probackup.h"

#include <sys/stat.h>
#include <unistd.h>

#include "utils/thread.h"

typedef struct
{
	parray	   *to_files;
	parray	   *files;
	parray	   *from_extra;

	pgBackup   *to_backup;
	pgBackup   *from_backup;
	const char *to_root;
	const char *from_root;
	const char *to_extra_prefix;
	const char *from_extra_prefix;

	/*
	 * Return value from the thread.
	 * 0 means there is no error, 1 - there is an error.
	 */
	int			ret;
} merge_files_arg;

static void merge_backups(pgBackup *backup, pgBackup *next_backup);
static void *merge_files(void *arg);
static void
reorder_extra_dirs(pgBackup *to_backup, parray *to_extra, parray *from_extra);
static int
get_extra_index(const char *key, const parray *list);

/*
 * Implementation of MERGE command.
 *
 * - Find target and its parent full backup
 * - Merge data files of target, parent and and intermediate backups
 * - Remove unnecessary files, which doesn't exist in the target backup anymore
 */
void
do_merge(time_t backup_id)
{
	parray	   *backups;
	pgBackup   *dest_backup = NULL;
	pgBackup   *full_backup = NULL;
	time_t		prev_parent = INVALID_BACKUP_ID;
	int			i;
	int			dest_backup_idx = 0;
	int			full_backup_idx = 0;

	if (backup_id == INVALID_BACKUP_ID)
		elog(ERROR, "required parameter is not specified: --backup-id");

	if (instance_name == NULL)
		elog(ERROR, "required parameter is not specified: --instance");

	elog(INFO, "Merge started");

	catalog_lock();

	/* Get list of all backups sorted in order of descending start time */
	backups = catalog_get_backup_list(INVALID_BACKUP_ID);

	/* Find destination and parent backups */
	for (i = 0; i < parray_num(backups); i++)
	{
		pgBackup   *backup = (pgBackup *) parray_get(backups, i);

		if (backup->start_time > backup_id)
			continue;
		else if (backup->start_time == backup_id && !dest_backup)
		{
			if (backup->status != BACKUP_STATUS_OK &&
				/* It is possible that previous merging was interrupted */
				backup->status != BACKUP_STATUS_MERGING &&
				backup->status != BACKUP_STATUS_DELETING)
				elog(ERROR, "Backup %s has status: %s",
					 base36enc(backup->start_time), status2str(backup->status));

			if (backup->backup_mode == BACKUP_MODE_FULL)
				elog(ERROR, "Backup %s is full backup",
					 base36enc(backup->start_time));

			dest_backup = backup;
			dest_backup_idx = i;
		}
		else
		{
			if (dest_backup == NULL)
				elog(ERROR, "Target backup %s was not found", base36enc(backup_id));

			if (backup->start_time != prev_parent)
				continue;

			if (backup->status != BACKUP_STATUS_OK &&
				/* It is possible that previous merging was interrupted */
				backup->status != BACKUP_STATUS_MERGING)
				elog(ERROR, "Backup %s has status: %s",
					 base36enc(backup->start_time), status2str(backup->status));

			/* If we already found dest_backup, look for full backup */
			if (dest_backup && backup->backup_mode == BACKUP_MODE_FULL)
			{
				full_backup = backup;
				full_backup_idx = i;

				/* Found target and full backups, so break the loop */
				break;
			}
		}

		prev_parent = backup->parent_backup;
	}

	if (dest_backup == NULL)
		elog(ERROR, "Target backup %s was not found", base36enc(backup_id));
	if (full_backup == NULL)
		elog(ERROR, "Parent full backup for the given backup %s was not found",
			 base36enc(backup_id));

	Assert(full_backup_idx != dest_backup_idx);

	/*
	 * Found target and full backups, merge them and intermediate backups
	 */
	for (i = full_backup_idx; i > dest_backup_idx; i--)
	{
		pgBackup   *from_backup = (pgBackup *) parray_get(backups, i - 1);

		merge_backups(full_backup, from_backup);
	}

	pgBackupValidate(full_backup);
	if (full_backup->status == BACKUP_STATUS_CORRUPT)
		elog(ERROR, "Merging of backup %s failed", base36enc(backup_id));

	/* cleanup */
	parray_walk(backups, pgBackupFree);
	parray_free(backups);

	elog(INFO, "Merge of backup %s completed", base36enc(backup_id));
}

/*
 * Merge two backups data files using threads.
 * - move instance files from from_backup to to_backup
 * - remove unnecessary directories and files from to_backup
 * - update metadata of from_backup, it becames FULL backup
 */
static void
merge_backups(pgBackup *to_backup, pgBackup *from_backup)
{
	char	   *to_backup_id = base36enc_dup(to_backup->start_time),
			   *from_backup_id = base36enc_dup(from_backup->start_time);
	char		to_backup_path[MAXPGPATH],
				to_database_path[MAXPGPATH],
				to_extra_prefix[MAXPGPATH],
				from_backup_path[MAXPGPATH],
				from_database_path[MAXPGPATH],
				from_extra_prefix[MAXPGPATH],
				control_file[MAXPGPATH];
	parray	   *files,
			   *to_files;
	parray	   *to_extra = NULL,
			   *from_extra = NULL;
	pthread_t  *threads = NULL;
	merge_files_arg *threads_args = NULL;
	int			i;
	bool		merge_isok = true;

	elog(INFO, "Merging backup %s with backup %s", from_backup_id, to_backup_id);

	/*
	 * Validate to_backup only if it is BACKUP_STATUS_OK. If it has
	 * BACKUP_STATUS_MERGING status then it isn't valid backup until merging
	 * finished.
	 */
	if (to_backup->status == BACKUP_STATUS_OK)
	{
		pgBackupValidate(to_backup);
		if (to_backup->status == BACKUP_STATUS_CORRUPT)
			elog(ERROR, "Interrupt merging");
	}

	/*
	 * It is OK to validate from_backup if it has BACKUP_STATUS_OK or
	 * BACKUP_STATUS_MERGING status.
	 */
	Assert(from_backup->status == BACKUP_STATUS_OK ||
		   from_backup->status == BACKUP_STATUS_MERGING);
	pgBackupValidate(from_backup);
	if (from_backup->status == BACKUP_STATUS_CORRUPT)
		elog(ERROR, "Interrupt merging");

	/*
	 * Make backup paths.
	 */
	pgBackupGetPath(to_backup, to_backup_path, lengthof(to_backup_path), NULL);
	pgBackupGetPath(to_backup, to_database_path, lengthof(to_database_path),
					DATABASE_DIR);
	pgBackupGetPath(to_backup, to_extra_prefix, lengthof(to_database_path),
					EXTRA_DIR);
	pgBackupGetPath(from_backup, from_backup_path, lengthof(from_backup_path), NULL);
	pgBackupGetPath(from_backup, from_database_path, lengthof(from_database_path),
					DATABASE_DIR);
	pgBackupGetPath(from_backup, from_extra_prefix, lengthof(from_database_path),
					EXTRA_DIR);

	/*
	 * Get list of files which will be modified or removed.
	 */
	pgBackupGetPath(to_backup, control_file, lengthof(control_file),
					DATABASE_FILE_LIST);
	to_files = dir_read_file_list(NULL, NULL, control_file);
	/* To delete from leaf, sort in reversed order */
	parray_qsort(to_files, pgFileComparePathDesc);
	/*
	 * Get list of files which need to be moved.
	 */
	pgBackupGetPath(from_backup, control_file, lengthof(control_file),
					DATABASE_FILE_LIST);
	files = dir_read_file_list(NULL, NULL, control_file);
	/* sort by size for load balancing */
	parray_qsort(files, pgFileCompareSize);

	/*
	 * Previous merging was interrupted during deleting source backup. It is
	 * safe just to delete it again.
	 */
	if (from_backup->status == BACKUP_STATUS_DELETING)
		goto delete_source_backup;

	to_backup->status = BACKUP_STATUS_MERGING;
	write_backup_status(to_backup);

	from_backup->status = BACKUP_STATUS_MERGING;
	write_backup_status(from_backup);

	create_data_directories(to_database_path, from_backup_path, false);

	threads = (pthread_t *) palloc(sizeof(pthread_t) * num_threads);
	threads_args = (merge_files_arg *) palloc(sizeof(merge_files_arg) * num_threads);

	/* Create extra directories lists */
	if (to_backup->extra_dir_str)
		to_extra = make_extra_directory_list(to_backup->extra_dir_str);
	if (from_backup->extra_dir_str)
		from_extra = make_extra_directory_list(from_backup->extra_dir_str);

	/*
	 * Rename extra directoties in to_backup (if exists)
	 * according to numeration of extra dirs in from_backup.
	 */
	if (to_extra)
		reorder_extra_dirs(to_backup, to_extra, from_extra);

	/* Setup threads */
	for (i = 0; i < parray_num(files); i++)
	{
		pgFile	   *file = (pgFile *) parray_get(files, i);

		/* if the entry was an extra directory, create it in the backup */
		if (file->extra_dir_num && S_ISDIR(file->mode))
		{
			char		dirpath[MAXPGPATH];
			char		new_container[MAXPGPATH];

			makeExtraDirPathByNum(new_container, to_extra_prefix,
								  file->extra_dir_num);
			join_path_components(dirpath, new_container, file->path);
			dir_create_dir(dirpath, DIR_PERMISSION);
		}
		pg_atomic_init_flag(&file->lock);
	}

	for (i = 0; i < num_threads; i++)
	{
		merge_files_arg *arg = &(threads_args[i]);

		arg->to_files = to_files;
		arg->files = files;
		arg->to_backup = to_backup;
		arg->from_backup = from_backup;
		arg->to_root = to_database_path;
		arg->from_root = from_database_path;
		arg->from_extra = from_extra;
		arg->to_extra_prefix = to_extra_prefix;
		arg->from_extra_prefix = from_extra_prefix;
		/* By default there are some error */
		arg->ret = 1;

		elog(VERBOSE, "Start thread: %d", i);

		pthread_create(&threads[i], NULL, merge_files, arg);
	}

	/* Wait threads */
	for (i = 0; i < num_threads; i++)
	{
		pthread_join(threads[i], NULL);
		if (threads_args[i].ret == 1)
			merge_isok = false;
	}
	if (!merge_isok)
		elog(ERROR, "Data files merging failed");

	/*
	 * Update to_backup metadata.
	 */
	to_backup->status = BACKUP_STATUS_OK;
	to_backup->parent_backup = INVALID_BACKUP_ID;
	to_backup->start_lsn = from_backup->start_lsn;
	to_backup->stop_lsn = from_backup->stop_lsn;
	to_backup->recovery_time = from_backup->recovery_time;
	to_backup->recovery_xid = from_backup->recovery_xid;

	pfree(to_backup->extra_dir_str);
	to_backup->extra_dir_str = from_backup->extra_dir_str;
	from_backup->extra_dir_str = NULL; /* For safe pgBackupFree() */
	/*
	 * If one of the backups isn't "stream" backup then the target backup become
	 * non-stream backup too.
	 */
	to_backup->stream = to_backup->stream && from_backup->stream;
	/* Compute summary of size of regular files in the backup */
	to_backup->data_bytes = 0;
	for (i = 0; i < parray_num(files); i++)
	{
		pgFile	   *file = (pgFile *) parray_get(files, i);

		if (S_ISDIR(file->mode))
			to_backup->data_bytes += 4096;
		/* Count the amount of the data actually copied */
		else if (S_ISREG(file->mode))
			to_backup->data_bytes += file->write_size;
	}
	/* compute size of wal files of this backup stored in the archive */
	if (!to_backup->stream)
		to_backup->wal_bytes = instance_config.xlog_seg_size *
			(to_backup->stop_lsn / instance_config.xlog_seg_size -
			 to_backup->start_lsn / instance_config.xlog_seg_size + 1);
	else
		to_backup->wal_bytes = BYTES_INVALID;

	write_backup_filelist(to_backup, files, from_database_path,
						  from_extra_prefix, NULL);
	write_backup(to_backup);

delete_source_backup:
	/*
	 * Files were copied into to_backup. It is time to remove source backup
	 * entirely.
	 */
	delete_backup_files(from_backup);

	/*
	 * Delete files which are not in from_backup file list.
	 */
	parray_qsort(files, pgFileComparePathDesc);
	for (i = 0; i < parray_num(to_files); i++)
	{
		pgFile	   *file = (pgFile *) parray_get(to_files, i);

		if (file->extra_dir_num &&
			backup_contains_extra(parray_get(to_extra, file->extra_dir_num - 1),
								  from_extra))
			/* Dir already removed*/
			continue;

		if (parray_bsearch(files, file, pgFileComparePathDesc) == NULL)
		{
			char		to_file_path[MAXPGPATH];
			char	   *prev_path;

			/* We need full path, file object has relative path */
			join_path_components(to_file_path, to_database_path, file->path);
			prev_path = file->path;
			file->path = to_file_path;

			pgFileDelete(file);
			elog(VERBOSE, "Deleted \"%s\"", file->path);

			file->path = prev_path;
		}
	}

	/*
	 * Rename FULL backup directory.
	 */
	elog(INFO, "Rename %s to %s", to_backup_id, from_backup_id);
	if (rename(to_backup_path, from_backup_path) == -1)
		elog(ERROR, "Could not rename directory \"%s\" to \"%s\": %s",
			 to_backup_path, from_backup_path, strerror(errno));

	/*
	 * Merging finished, now we can safely update ID of the destination backup.
	 */
	to_backup->start_time = from_backup->start_time;
	write_backup(to_backup);

	/* Cleanup */
	if (threads)
	{
		pfree(threads_args);
		pfree(threads);
	}

	parray_walk(to_files, pgFileFree);
	parray_free(to_files);

	parray_walk(files, pgFileFree);
	parray_free(files);

	pfree(to_backup_id);
	pfree(from_backup_id);
}

/*
 * Thread worker of merge_backups().
 */
static void *
merge_files(void *arg)
{
	merge_files_arg *argument = (merge_files_arg *) arg;
	pgBackup   *to_backup = argument->to_backup;
	pgBackup   *from_backup = argument->from_backup;
	int			i,
				num_files = parray_num(argument->files);

	for (i = 0; i < num_files; i++)
	{
		pgFile	   *file = (pgFile *) parray_get(argument->files, i);
		pgFile	   *to_file;
		pgFile	  **res_file;
		char		from_file_path[MAXPGPATH];
		char	   *prev_file_path;

		if (!pg_atomic_test_set_flag(&file->lock))
			continue;

		/* check for interrupt */
		if (interrupted)
			elog(ERROR, "Interrupted during merging backups");

		/* Directories were created before */
		if (S_ISDIR(file->mode))
			continue;

		if (progress)
			elog(INFO, "Progress: (%d/%d). Process file \"%s\"",
				 i + 1, num_files, file->path);

		res_file = parray_bsearch(argument->to_files, file,
								  pgFileComparePathDesc);
		to_file = (res_file) ? *res_file : NULL;

		/*
		 * Skip files which haven't changed since previous backup. But in case
		 * of DELTA backup we should consider n_blocks to truncate the target
		 * backup.
		 */
		if (file->write_size == BYTES_INVALID && file->n_blocks == -1)
		{
			elog(VERBOSE, "Skip merging file \"%s\", the file didn't change",
				 file->path);

			/*
			 * If the file wasn't changed in PAGE backup, retreive its
			 * write_size from previous FULL backup.
			 */
			if (to_file)
			{
				file->compress_alg = to_file->compress_alg;
				file->write_size = to_file->write_size;
				file->crc = to_file->crc;
			}

			continue;
		}

		/* We need to make full path, file object has relative path */
		if (file->extra_dir_num)
		{
			char temp[MAXPGPATH];
			makeExtraDirPathByNum(temp, argument->from_extra_prefix,
								  file->extra_dir_num);

			join_path_components(from_file_path, temp, file->path);
		}
		else
			join_path_components(from_file_path, argument->from_root,
								 file->path);
		prev_file_path = file->path;
		file->path = from_file_path;

		/*
		 * Move the file. We need to decompress it and compress again if
		 * necessary.
		 */
		elog(VERBOSE, "Merging file \"%s\", is_datafile %d, is_cfs %d",
			 file->path, file->is_database, file->is_cfs);

		if (file->is_datafile && !file->is_cfs)
		{
			char		to_file_path[MAXPGPATH];	/* Path of target file */

			join_path_components(to_file_path, argument->to_root, prev_file_path);

			/*
			 * We need more complicate algorithm if target file should be
			 * compressed.
			 */
			if (to_backup->compress_alg == PGLZ_COMPRESS ||
				to_backup->compress_alg == ZLIB_COMPRESS)
			{
				char		tmp_file_path[MAXPGPATH];
				char	   *prev_path;

				snprintf(tmp_file_path, MAXPGPATH, "%s_tmp", to_file_path);

				/* Start the magic */

				/*
				 * Merge files:
				 * - if target file exists restore and decompress it to the temp
				 *   path
				 * - decompress source file if necessary and merge it with the
				 *   target decompressed file
				 * - compress result file
				 */

				/*
				 * We need to decompress target file if it exists.
				 */
				if (to_file)
				{
					elog(VERBOSE, "Merge target and source files into the temporary path \"%s\"",
						 tmp_file_path);

					/*
					 * file->path points to the file in from_root directory. But we
					 * need the file in directory to_root.
					 */
					prev_path = to_file->path;
					to_file->path = to_file_path;
					/* Decompress target file into temporary one */
					restore_data_file(tmp_file_path, to_file, false, false,
									  parse_program_version(to_backup->program_version));
					to_file->path = prev_path;
				}
				else
					elog(VERBOSE, "Restore source file into the temporary path \"%s\"",
						 tmp_file_path);
				/* Merge source file with target file */
				restore_data_file(tmp_file_path, file,
								  from_backup->backup_mode == BACKUP_MODE_DIFF_DELTA,
								  false,
								  parse_program_version(from_backup->program_version));

				elog(VERBOSE, "Compress file and save it into the directory \"%s\"",
					 argument->to_root);

				/* Again we need to change path */
				prev_path = file->path;
				file->path = tmp_file_path;
				/* backup_data_file() requires file size to calculate nblocks */
				file->size = pgFileSize(file->path);
				/* Now we can compress the file */
				backup_data_file(NULL, /* We shouldn't need 'arguments' here */
								 to_file_path, file,
								 to_backup->start_lsn,
								 to_backup->backup_mode,
								 to_backup->compress_alg,
								 to_backup->compress_level);

				file->path = prev_path;

				/* We can remove temporary file now */
				if (unlink(tmp_file_path))
					elog(ERROR, "Could not remove temporary file \"%s\": %s",
						 tmp_file_path, strerror(errno));
			}
			/*
			 * Otherwise merging algorithm is simpler.
			 */
			else
			{
				/* We can merge in-place here */
				restore_data_file(to_file_path, file,
								  from_backup->backup_mode == BACKUP_MODE_DIFF_DELTA,
								  true,
								  parse_program_version(from_backup->program_version));

				/*
				 * We need to calculate write_size, restore_data_file() doesn't
				 * do that.
				 */
				file->write_size = pgFileSize(to_file_path);
				file->crc = pgFileGetCRC(to_file_path, true, true, NULL);
			}
		}
		else if (strcmp(file->name, "pg_control") == 0)
			copy_pgcontrol_file(argument->from_root, argument->to_root, file);
		else if (file->extra_dir_num)
		{
			char	from_root[MAXPGPATH];
			char	to_root[MAXPGPATH];
			int		new_dir_num;
			char   *file_extra_path = parray_get(argument->from_extra,
												 file->extra_dir_num - 1);

			Assert(argument->from_extra);
			new_dir_num = get_extra_index(file_extra_path,
										  argument->from_extra);
			makeExtraDirPathByNum(from_root, argument->from_extra_prefix,
								  file->extra_dir_num);
			makeExtraDirPathByNum(to_root, argument->to_extra_prefix,
								  new_dir_num);
			copy_file(from_root, to_root, file);
		}
		else
			copy_file(argument->from_root, argument->to_root, file);

		/*
		 * We need to save compression algorithm type of the target backup to be
		 * able to restore in the future.
		 */
		file->compress_alg = to_backup->compress_alg;

		if (file->write_size != BYTES_INVALID)
			elog(LOG, "Merged file \"%s\": " INT64_FORMAT " bytes",
				 file->path, file->write_size);

		/* Restore relative path */
		file->path = prev_file_path;
	}

	/* Data files merging is successful */
	argument->ret = 0;

	return NULL;
}

/* Recursively delete a directory and its contents */
static void
remove_dir_with_files(const char *path)
{
	parray *files = parray_new();
	dir_list_file(files, path, true, true, true, 0);
	parray_qsort(files, pgFileComparePathDesc);
	for (int i = 0; i < parray_num(files); i++)
	{
		pgFile	   *file = (pgFile *) parray_get(files, i);

		pgFileDelete(file);
		elog(VERBOSE, "Deleted \"%s\"", file->path);
	}
}

/* Get index of extra directory */
static int
get_extra_index(const char *key, const parray *list)
{
	if (!list) /* Nowhere to search */
		return -1;
	for (int i = 0; i < parray_num(list); i++)
	{
		if (strcmp(key, parray_get(list, i)) == 0)
			return i + 1;
	}
	return -1;
}

/* Rename directories in to_backup according to order in from_extra */
static void
reorder_extra_dirs(pgBackup *to_backup, parray *to_extra, parray *from_extra)
{
	char extradir_template[MAXPGPATH];

	pgBackupGetPath(to_backup, extradir_template,
					lengthof(extradir_template), EXTRA_DIR);
	for (int i = 0; i < parray_num(to_extra); i++)
	{
		int from_num = get_extra_index(parray_get(to_extra, i), from_extra);
		if (from_num == -1)
		{
			char old_path[MAXPGPATH];
			makeExtraDirPathByNum(old_path, extradir_template, i + 1);
			remove_dir_with_files(old_path);
		}
		else if (from_num != i + 1)
		{
			char old_path[MAXPGPATH];
			char new_path[MAXPGPATH];
			makeExtraDirPathByNum(old_path, extradir_template, i + 1);
			makeExtraDirPathByNum(new_path, extradir_template, from_num);
			elog(VERBOSE, "Rename %s to %s", old_path, new_path);
			if (rename (old_path, new_path) == -1)
				elog(ERROR, "Could not rename directory \"%s\" to \"%s\": %s",
					 old_path, new_path, strerror(errno));
		}
	}
}
