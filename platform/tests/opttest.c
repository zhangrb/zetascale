//----------------------------------------------------------------------------
// ZetaScale
// Copyright (c) 2016, SanDisk Corp. and/or all its affiliates.
//
// This program is free software; you can redistribute it and/or modify it under
// the terms of the GNU Lesser General Public License version 2.1 as published by the Free
// Software Foundation;
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License v2.1 for more details.
//
// A copy of the GNU Lesser General Public License v2.1 is provided with this package and
// can also be found at: http://opensource.org/licenses/LGPL-2.1
// You should have received a copy of the GNU Lesser General Public License along with
// this program; if not, write to the Free Software Foundation, Inc., 59 Temple
// Place, Suite 330, Boston, MA 02111-1307 USA.
//----------------------------------------------------------------------------

#include "platform/string.h" 

#define PLAT_OPTS_NAME(name) name ## _opttest

#include "platform/opts.h"

#include "misc/misc.h"

#define PLAT_OPTS_ITEMS_opttest()                                              \
    item("string", "string arg", STRING,                                       \
         parse_string_alloc(&config->string_arg, optarg, PATH_MAX),            \
         PLAT_OPTS_ARG_REQUIRED)                                               \
    item("int", "int arg", INT,                                                \
         parse_int(&config->int_arg, optarg, NULL), PLAT_OPTS_ARG_REQUIRED)    \
    item("required", "required", REQUIRED,                                     \
         0 /* success */, (PLAT_OPTS_OPT_REQUIRED|PLAT_OPTS_ARG_NO))

struct plat_opts_config_opttest {
    char *string_arg;
    int int_arg;
};

int 
main(int argc, char *argv[])
{
    char *good_argv[] = { "good",  "--string", "a string", "--int", "42",
        "--required", NULL };
    int good_argc = sizeof (good_argv) / sizeof (good_argv[0]) - 1;
    char *bad_argv[] = { "bad", "--string", "--required", NULL };
    int bad_argc = sizeof (bad_argv) / sizeof (bad_argv[0]) - 1;
    char *bad2_argv[] = { "bad2", NULL };
    int bad2_argc = sizeof (bad2_argv) / sizeof (bad2_argv[0]) - 1;

    int status;

    struct plat_opts_config_opttest config;

    memset (&config, 0, sizeof(config));
    status = plat_opts_parse_opttest(&config, good_argc, good_argv);
    plat_assert_always(!status);
    plat_assert_always(strcmp(config.string_arg, "a string") == 0);
    plat_assert_always(config.int_arg == 42);
    parse_string_free(config.string_arg);

    memset (&config, 0, sizeof(config));
    status = plat_opts_parse_opttest(&config, bad_argc, bad_argv);
    plat_assert_always(status);

    memset (&config, 0, sizeof(config));
    status = plat_opts_parse_opttest(&config, bad2_argc, bad2_argv);
    plat_assert_always(status);

    return (0);
}

#include "platform/opts_c.h"
