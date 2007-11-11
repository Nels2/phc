/*
 * phc -- the open source PHP compiler
 * See doc/license/README.license for licensing information
 *
 * Main application module 
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <ltdl.h>
#include "cmdline.h"
#include "AST.h"
#include "parsing/parse.h"
#include "embed/embed.h"
#include "process_ast/Invalid_check.h"
#include "process_ast/Pretty_print.h"
#include "process_ast/PHP_unparser.h"
#include "process_ast/XML_unparser.h"
#include "process_ast/DOT_unparser.h"
#include "process_ast/Remove_concat_null.h"
#include "process_ast/Process_includes.h"
#include "process_ast/Note_top_level_declarations.h"
#include "ast_to_hir/Lower_control_flow.h"
#include "ast_to_hir/Lower_expr_flow.h"
#include "ast_to_hir/Shredder.h"
#include "ast_to_hir/Split_multiple_arguments.h"
#include "process_hir/Obfuscate.h"
#include "codegen/Strip_comments.h"
#include "codegen/Lift_functions_and_classes.h"
#include "codegen/Prune_symbol_table.h"
#include "codegen/Clarify.h"
#include "codegen/Generate_C.h"
#include "codegen/Compile_C.h"
#include "pass_manager/Fake_pass.h"
#include "pass_manager/Pass_manager.h"

using namespace std;

void init_plugins (Pass_manager* pm);

void sighandler(int signum)
{
	switch(signum)
	{
		case SIGABRT:
			fprintf(stderr, "SIGABRT received!\n");
			break;
		case SIGSEGV:
			fprintf(stderr, "SIGSEGV received!\n");
			break;
		default:
			fprintf(stderr, "Unknown signal received!\n");
			break;
	}

	fprintf(stderr, "This could be a bug in phc. If you suspect it is, please email\n");
	fprintf(stderr, "a bug report to phc-general@phpcompiler.org.\n");
	exit(-1);
}



extern struct gengetopt_args_info error_args_info;
struct gengetopt_args_info args_info;

int main(int argc, char** argv)
{

	/* 
	 *	Startup
	 */

	AST_php_script* php_script = NULL;

	// Start the embedded interpreter
	PHP::startup_php ();

	// Synchronise C and C++ I/O
	ios_base::sync_with_stdio();

	// catch a seg fault - note this doesnt harm gdb, which will override this.
	// We do this so that tests will still continue if theres a seg fault
	signal(SIGSEGV, sighandler);
	signal(SIGABRT, sighandler);

	// Parse command line parameters 
	if(cmdline_parser(argc, argv, &args_info) != 0)
		exit(-1);

	// Passign this struct through the pass manager is a bit hard.
	error_args_info = args_info;

	// The rest of the compilation is controlled by the pass manager.
	Pass_manager* pm = new Pass_manager (&args_info);

	/* 
	 *	Parsing 
	 */
	if(args_info.inputs_num == 0)
	{
		php_script = parse(new String("-"), NULL, args_info.read_ast_xml_flag);
	}
	else
	{
		if (args_info.inputs_num > 1)
			phc_error ("Only 1 input file can be processed. Input file '%s' is ignored.", args_info.inputs[1]);

		php_script = parse(new String(args_info.inputs[0]), NULL, args_info.read_ast_xml_flag);
		if (php_script == NULL)
		{
			phc_error("File not found", new String(args_info.inputs[0]), 0);
		}
	}

	if(php_script == NULL) return -1;

	// Make sure the inputs cannot be accessed globally
	args_info.inputs = NULL;

	/* 
	 *	Compilation
	 */

	// process_ast passes
	pm->add_pass (new List_passes ());
	pm->add_pass (new Invalid_check ());
	// TODO add flag for include 
	pm->add_pass (new Process_includes (false, new String ("ast"), pm, "incl1"));
	pm->add_pass (new Fake_pass ("ast"));
	pm->add_pass (new Pretty_print ());
	pm->add_visitor (new Note_top_level_declarations (), "ntld");

	// ast_to_hir passes
	pm->add_transform (new Remove_concat_null (), "rcn");
	pm->add_transform (new Split_multiple_arguments (), "sma");
	pm->add_pass (new Lift_functions_and_classes ()); // codegen
	pm->add_transform (new Split_unset_isset (), "sui");
	pm->add_transform (new Translate_empty (), "empty");
	pm->add_transform (new Lower_control_flow (), "lcf");
	pm->add_transform (new Lower_expr_flow (), "lef");
	pm->add_transform (new Echo_split (), "ecs");
	pm->add_transform (new Pre_post_op_shredder (), "pps");
	pm->add_transform (new Desugar (), "desug");
	pm->add_transform (new List_shredder (), "lish");
	pm->add_transform (new Shredder (), "shred");
	pm->add_transform (new Tidy_print (), "tidyp");
	pm->add_pass (new Process_includes (true, new String ("hir"), pm, "incl2"));

	// process_hir passes
	pm->add_pass (new Fake_pass ("hir"));
	pm->add_visitor (new Strip_comments (), "decomment"); // codegen
	pm->add_pass (new Obfuscate ());

	// codegen passes
	// Use ss to pass generated code between Generate_C and Compile_C
	stringstream ss;
	pm->add_visitor (new Clarify (), "clar");
	pm->add_visitor (new Prune_symbol_table (), "pst");
	pm->add_pass (new Generate_C (ss));
	pm->add_pass (new Compile_C (ss));


	// Plugins add their passes to the pass manager
	init_plugins (pm);


	// Launch the pass manager
	pm->run (php_script);
	pm->post_process ();

	/*
	 * Destruction
	 */
	int ret = lt_dlexit();
	if (ret != 0) 
		phc_error ("Error closing ltdl plugin infrastructure: %s", lt_dlerror ());

	PHP::shutdown_php ();

	return 0;
}
		

void init_plugins (Pass_manager* pm)
{
	int ret = lt_dlinit();
	if (ret != 0) phc_error ("Error initializing ltdl plugin infrastructure: %s", lt_dlerror ());

	for(unsigned i = 0; i < pm->args_info->run_given; i++)
	{
		// Try opening the specified plugin from its default location, 
		// from the current working directory, and from PKGLIBDIR (in that order)
		const char* name = pm->args_info->run_arg[i];
		const char* option = "";
		if (pm->args_info->r_option_given >= i+1)
			option = pm->args_info->r_option_arg[i];
		lt_dlhandle handle = lt_dlopenext (name);

		const char* default_err;
		const char* cwd_err;
		char in_cwd[256];
		if(handle == NULL)
		{
			// we record multiple errors, so need strdup, or they'll oerv-write each other
			default_err = strdup (lt_dlerror ()); 

			// Try current directory
			getcwd(in_cwd, 256);

			// TODO insecure
			strcat(in_cwd, "/");
			strcat(in_cwd, name);
			handle = lt_dlopenext (in_cwd);
		}

		const char* datadir_err;
		char in_datadir[256];
		if(handle == NULL)
		{
			cwd_err = strdup (lt_dlerror ());

			// Try installed dir
			// TODO insecure 
			strcpy(in_datadir, PKGLIBDIR);
			strcat(in_datadir, "/");
			strcat(in_datadir, name);
			handle = lt_dlopenext (in_datadir);	
		}

		if(handle == NULL)
		{
			datadir_err = strdup (lt_dlerror ());
			phc_error (
				"Could not find %s plugin with errors \"%s\", \"%s\" and \"%s\"",
				NULL, 0, name, default_err, cwd_err, datadir_err);
		}

		// Save for later
		pm->add_plugin (handle, name, new String (option));

	}
}
