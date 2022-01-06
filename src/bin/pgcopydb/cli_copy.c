/*
 * src/bin/pgcopydb/cli_copy.c
 *     Implementation of a CLI which lets you run individual routines
 *     directly
 */

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>

#include "cli_common.h"
#include "cli_copy.h"
#include "cli_root.h"
#include "copydb.h"
#include "commandline.h"
#include "log.h"
#include "pgsql.h"
#include "string_utils.h"
#include "summary.h"

CopyDBOptions copyDBoptions = { 0 };

static int cli_copy_db_getopts(int argc, char **argv);
static void cli_copy_db(int argc, char **argv);


static CommandLine copy_db_command =
	make_command(
		"db",
		"Copy an entire database from source to target",
		" --source ... --target ... [ --jobs ] ",
		"  --source          Postgres URI to the source database\n"
		"  --target          Postgres URI to the target database\n"
		"  --table-jobs      Number of concurrent COPY jobs to run\n"
		"  --index-jobs      Number of concurrent CREATE INDEX jobs to run\n",
		cli_copy_db_getopts,
		cli_copy_db);

static CommandLine copy_table_command =
	make_command(
		"table",
		"Copy a given table from source to target",
		" --source ... --target ... [ --jobs ] ",
		"  --source          Postgres URI to the source database\n"
		"  --target          Postgres URI to the target database\n"
		"  --schema-name     Name of the schema where to find the table\n"
		"  --table-name      Name of the target table\n"
		"  --table-jobs      Number of concurrent COPY jobs to run\n"
		"  --index-jobs      Number of concurrent CREATE INDEX jobs to run\n",
		cli_copy_db_getopts,
		cli_copy_db);


static CommandLine *copy_subcommands[] = {
	&copy_db_command,
	&copy_table_command,
	NULL
};

CommandLine copy_commands =
	make_command_set("copy",
					 "Copy database objects from a Postgres instance to another",
					 NULL, NULL, NULL, copy_subcommands);


/*
 * cli_copy_db_getopts parses the CLI options for the `copy db` command.
 */
static int
cli_copy_db_getopts(int argc, char **argv)
{
	CopyDBOptions options = { 0 };
	int c, option_index = 0;
	int verboseCount = 0;

	static struct option long_options[] = {
		{ "source", required_argument, NULL, 'S' },
		{ "target", required_argument, NULL, 'T' },
		{ "jobs", required_argument, NULL, 'J' },
		{ "table-jobs", required_argument, NULL, 'J' },
		{ "index-jobs", required_argument, NULL, 'I' },
		{ "schema", required_argument, NULL, 's' },
		{ "table", required_argument, NULL, 't' },
		{ "version", no_argument, NULL, 'V' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "quiet", no_argument, NULL, 'q' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	optind = 0;

	/* install default values */
	options.tableJobs = 4;
	options.indexJobs = 4;

	strlcpy(options.schema_name, "public", NAMEDATALEN);

	while ((c = getopt_long(argc, argv, "S:T:j:s:t:Vvqh",
							long_options, &option_index)) != -1)
	{
		switch (c)
		{
			case 'S':
			{
				if (!validate_connection_string(optarg))
				{
					log_fatal("Failed to parse --monitor connection string, "
							  "see above for details.");
					exit(EXIT_CODE_BAD_ARGS);
				}
				strlcpy(options.source_pguri, optarg, MAXCONNINFO);
				log_trace("--source %s", options.source_pguri);
				break;
			}

			case 'T':
			{
				if (!validate_connection_string(optarg))
				{
					log_fatal("Failed to parse --monitor connection string, "
							  "see above for details.");
					exit(EXIT_CODE_BAD_ARGS);
				}
				strlcpy(options.target_pguri, optarg, MAXCONNINFO);
				log_trace("--target %s", options.target_pguri);
				break;
			}

			case 'J':
			{
				if (!stringToInt(optarg, &options.tableJobs) ||
					options.tableJobs < 1 ||
					options.tableJobs > 128)
				{
					log_fatal("Failed to parse --jobs count: \"%s\"", optarg);
					exit(EXIT_CODE_BAD_ARGS);
				}
				log_trace("--table-jobs %d", options.tableJobs);
				break;
			}

			case 'I':
			{
				if (!stringToInt(optarg, &options.indexJobs) ||
					options.indexJobs < 1 ||
					options.indexJobs > 128)
				{
					log_fatal("Failed to parse --index-jobs count: \"%s\"", optarg);
					exit(EXIT_CODE_BAD_ARGS);
				}
				log_trace("--jobs %d", options.indexJobs);
				break;
			}

			case 's':
			{
				strlcpy(options.schema_name, optarg, NAMEDATALEN);
				log_trace("--schema %s", options.schema_name);
				break;
			}

			case 't':
			{
				strlcpy(options.table_name, optarg, NAMEDATALEN);
				log_trace("--table %s", options.table_name);
				break;
			}

			case 'V':
			{
				/* keeper_cli_print_version prints version and exits. */
				cli_print_version(argc, argv);
				break;
			}

			case 'v':
			{
				++verboseCount;
				switch (verboseCount)
				{
					case 1:
					{
						log_set_level(LOG_INFO);
						break;
					}

					case 2:
					{
						log_set_level(LOG_DEBUG);
						break;
					}

					default:
					{
						log_set_level(LOG_TRACE);
						break;
					}
				}
				break;
			}

			case 'q':
			{
				log_set_level(LOG_ERROR);
				break;
			}

			case 'h':
			{
				commandline_help(stderr);
				exit(EXIT_CODE_QUIT);
				break;
			}
		}
	}

	/* publish our option parsing in the global variable */
	copyDBoptions = options;

	return optind;
}


/*
 * cli_copy_db implements the command: pgcopydb copy db
 */
static void
cli_copy_db(int argc, char **argv)
{
	Summary summary = { 0 };
	TopLevelTimings *timings = &(summary.timings);

	CopyFilePaths cfPaths = { 0 };
	PostgresPaths pgPaths = { 0 };

	(void) summary_set_current_time(timings, TIMING_STEP_START);

	log_info("[SOURCE] Copying database from \"%s\"", copyDBoptions.source_pguri);
	log_info("[TARGET] Copying database into \"%s\"", copyDBoptions.target_pguri);

	(void) find_pg_commands(&pgPaths);

	if (!copydb_init_workdir(&cfPaths, NULL))
	{
		/* errors have already been logged */
		exit(EXIT_CODE_INTERNAL_ERROR);
	}

	CopyDataSpec copySpecs = { 0 };

	if (!copydb_init_specs(&copySpecs,
						   &cfPaths,
						   &pgPaths,
						   copyDBoptions.source_pguri,
						   copyDBoptions.target_pguri,
						   copyDBoptions.tableJobs,
						   copyDBoptions.indexJobs))
	{
		/* errors have already been logged */
		exit(EXIT_CODE_INTERNAL_ERROR);
	}

	log_info("STEP 1: dump the source database schema (pre/post data)");

	(void) summary_set_current_time(timings, TIMING_STEP_BEFORE_SCHEMA_DUMP);

	if (!copydb_dump_source_schema(&copySpecs))
	{
		/* errors have already been logged */
		exit(EXIT_CODE_INTERNAL_ERROR);
	}

	log_info("STEP 2: restore the pre-data section to the target database");

	(void) summary_set_current_time(timings, TIMING_STEP_BEFORE_PREPARE_SCHEMA);

	if (!copydb_target_prepare_schema(&copySpecs))
	{
		/* errors have already been logged */
		exit(EXIT_CODE_TARGET);
	}

	(void) summary_set_current_time(timings, TIMING_STEP_AFTER_PREPARE_SCHEMA);

	log_info("STEP 3: copy data from source to target in sub-processes");
	log_info("STEP 4: create indexes and constraints in parallel");
	log_info("STEP 5: vacuum analyze each table");

	if (!copydb_copy_all_table_data(&copySpecs))
	{
		/* errors have already been logged */
		exit(EXIT_CODE_INTERNAL_ERROR);
	}

	log_info("STEP 6: restore the post-data section to the target database");

	(void) summary_set_current_time(timings, TIMING_STEP_BEFORE_FINALIZE_SCHEMA);

	if (!copydb_target_finalize_schema(&copySpecs))
	{
		/* errors have already been logged */
		exit(EXIT_CODE_TARGET);
	}

	(void) summary_set_current_time(timings, TIMING_STEP_AFTER_FINALIZE_SCHEMA);

	(void) print_summary(&summary, &copySpecs);
}
