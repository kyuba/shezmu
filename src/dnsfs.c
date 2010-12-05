/*
 * This file is part of the kyuba.org Shezmu project.
 * See the appropriate repository at http://git.kyuba.org/ for exact file
 * modification records.
*/

/*
 * Copyright (c) 2010, Kyuba Project Members
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
*/

#define _BSD_SOURCE

#include <stdlib.h>

#include <curie/multiplex.h>
#include <curie/memory.h>
#include <curie/directory.h>

#include <duat/9p-server.h>
#include <duat/filesystem.h>

#include <dnsfs/version.h>

#include <syscall/syscall.h>

#define HELPTEXT\
        dnsfs_version_long "\n"\
        "Usage: dnsfs [-ofih] [-s socket-name]\n"\
        "\n"\
        " -o          Talk 9p on stdio\n"\
        " -s          Talk 9p on the supplied socket-name\n"\
        " -h          Print this and exit.\n"\
        " -f          Don't detach and creep into the background.\n"\
        "\n"\
        " socket-name The socket to use.\n"\
        "\n"\
        "One of -s or -o must be specified.\n"\
        "\n"\
        "The programme will automatically fork to the background, unless -o is used.\n"\
        "\n"\

static struct sexpr_io *queue;
static struct io *queue_io;

define_symbol (sym_disable, "disable");

static void mx_sx_ctl_queue_read (sexpr sx, struct sexpr_io *io, void *aux)
{
    if (consp(sx))
    {
        sexpr sxcar = car (sx);
        if (truep(equalp(sxcar, sym_disable)))
        {
            exit (0);
        }
    }
}

static int_32 on_control_write
        (struct dfs_file *f, int_64 offset, int_32 length, int_8 *data)
{
    io_write (queue_io, (char *)data, length);

    return length;
}

static void print_help()
{
    sys_write (1, HELPTEXT, sizeof (HELPTEXT));
    exit(0);
}

int main (int argc, char **argv, char **environ)
{
    int i;
    struct dfs *fs;
    char use_stdio = 0;
    char *use_socket = (char *)0;
    char next_socket = 0;
    char o_foreground = 0;

    multiplex_io();

    multiplex_sexpr();

    for (i = 1; argv[i]; i++)
    {
        if (argv[i][0] == '-')
        {
            int j;
            for (j = 1; argv[i][j] != (char)0; j++)
	    {
                switch (argv[i][j])
                {
                    case 'o': use_stdio = 1; break;
                    case 's': next_socket = 1; break;
                    case 'f': o_foreground = 1; break;
                    default:
                        print_help();
                }
            }
            continue;
        }

        if (next_socket)
        {
            use_socket = argv[i];
            next_socket = 0;
            continue;
        }
    }

    if ((use_socket == (char *)0) && (use_stdio == 0))
    {
        print_help();
    }

    fs = dfs_create ((void *)0, (void *)0);
    fs->root->c.mode |= 0111;

    struct dfs_directory *d_dnsfs = dfs_mk_directory (fs->root, "dnsfs");
    struct dfs_file *d_dnsfs_ctl  = dfs_mk_file (d_dnsfs, "control", (char *)0,
            (int_8 *)"(nop)\n", 6, (void *)0, (void *)0, on_control_write);

    queue_io = io_open_special();
    d_dnsfs->c.mode     = 0550;
    d_dnsfs->c.uid      = "dnsfs";
    d_dnsfs->c.gid      = "dnsfs";
    d_dnsfs_ctl->c.mode = 0660;
    d_dnsfs_ctl->c.uid  = "dnsfs";
    d_dnsfs_ctl->c.gid  = "dnsfs";

    queue = sx_open_i (queue_io);

    multiplex_add_sexpr (queue, mx_sx_ctl_queue_read, (void *)0);

    multiplex_all_processes();

    multiplex_d9s();

    if (use_stdio)
    {
        multiplex_add_d9s_stdio (fs);
    }
    else if (o_foreground == 0)
    {
        struct exec_context *context
                = execute(EXEC_CALL_NO_IO, (char **)0, (char **)0);

        switch (context->pid)
        {
            case -1:
                exit (11);
            case 0:
                break;
            default:
                exit (0);
        }
    }

    if (use_socket != (char *)0) {
        multiplex_add_d9s_socket (use_socket, fs);
    }

    while (multiplex() != mx_nothing_to_do);

    return 0;
}
