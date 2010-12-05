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
#define _POSIX_C_SOURCE 1

#include <stdlib.h>

#include <curie/multiplex.h>
#include <curie/network.h>
#include <curie/memory.h>
#include <curie/directory.h>

#include <sievert/tree.h>

#include <duat/9p-server.h>
#include <duat/filesystem.h>

#include <dnsfs/version.h>

#include <syscall/syscall.h>

#include <sys/socket.h>
#include <netdb.h>

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

static void Tattach (struct d9r_io *io, int_16 tag, int_32 fid, int_32 afid,
                     char *uname, char *aname)
{
    struct d9r_fid_metadata *md = d9r_fid_metadata (io, fid);
    struct dfs *fs = (struct dfs *)io->aux;
    struct d9r_qid qid = { 0, 1, (int_64)(int_pointer)io->aux };

    if (md != (struct d9r_fid_metadata *)0)
    {
        md->aux = fs->root;
    }

    d9r_reply_attach (io, tag, qid);
}

static void Twalk (struct d9r_io *io, int_16 tag, int_32 fid, int_32 afid,
                   int_16 c, char **names)
{
    struct dfs *fs = io->aux;
    struct d9r_qid qid[c];
    struct d9r_fid_metadata *md = d9r_fid_metadata (io, fid);
    struct dfs_directory *d;

    if (md != (struct d9r_fid_metadata *)0)
    {
        d = md->aux;
    }
    else
    {
        d = fs->root;
    }

    int_16 i = 0;

    while (i < c) {
        if (d->c.type == dft_directory) {
            struct tree_node *node;
            if (names[i][0] == 0)
            {
                goto ret;
            }
            if (names[i][0] == '.')
            {
                if (names[i][1] == 0) {
                    goto ret;
                } else if ((names[i][1] == '.') && (names[i][2] == 0)) {
                    d = d->parent;
                    goto ret;
                }
            }

            node = tree_get_node_string(d->nodes, names[i]);

            if (node == (struct tree_node *)0)
            {
                d9r_reply_error (io, tag, "No such file or directory", P9_EDONTCARE);
                return;
            }

            if ((d = node_get_value(node)) == (struct dfs_directory *)0)
            {
                d9r_reply_error (io, tag, "Internal Duat VFS issue", P9_EDONTCARE);
                return;
            }

            ret:

            qid[i].type    = 0;
            qid[i].version = 1;
            qid[i].path    = (int_64)(int_pointer)d;

            i++;
        } else {
            break;
        }
    }

    if ((i == 0) && (c == 1))
    {
        d9r_reply_error (io, tag, "No such file or directory", P9_EDONTCARE);
        return;
    }

    if (i == c)
    {
        md = d9r_fid_metadata (io, afid);
        md->aux = d;
    }
    else
    {
        d9r_reply_error (io, tag, "No such file or directory", P9_EDONTCARE);
        return;
    }

    d9r_reply_walk (io, tag, i, qid);
}

static void Tstat (struct d9r_io *io, int_16 tag, int_32 fid)
{
    struct d9r_fid_metadata *md = d9r_fid_metadata (io, fid);
    struct dfs_node_common *c = md->aux;
    struct d9r_qid qid = { 0, 1, (int_64)(int_pointer)c };
    int_32 modex = 0;
    char *ex = (char *)0;
    char devbuffer[10];

    switch (c->type)
    {
        case dft_directory:
            qid.type = QTDIR;
            modex = DMDIR;
            break;
        case dft_symlink:
            qid.type = QTLINK;
            modex = DMSYMLINK;
            {
                struct dfs_symlink *link = (struct dfs_symlink *)c;
                ex = link->symlink;
            }
            break;
        case dft_device:
            modex = DMDEVICE;
            ex = devbuffer;

            {
                struct dfs_device *dev = (struct dfs_device *)c;
                int_16 i = 2, tc = 0;

                devbuffer[0] = (dev->type == dfs_block_device) ? 'b' : 'c';
                devbuffer[1] = ' ';

                if (dev->majour == 0)
                {
                    devbuffer[i] = '0';
                    i++;
                }
                else
                {
                    int_16 m = dev->majour, xi = i-1;
                    if (m >= 100) tc = 3;
                    else if (m >= 10) tc = 2;
                    else tc = 1;

                    i += tc;

                    while ((tc > 0) && (m != 0))
                    {
                        devbuffer[xi+tc] = '0' + (char)(m % 10);
                        m /= 10;
                        tc--;
                    }
                }

                devbuffer[i] = ' ';
                i++;

                if (dev->minor == 0)
                {
                    devbuffer[i] = '0';
                    i++;
                }
                else
                {
                    int_16 m = dev->minor, xi = i-1;
                    if (m >= 100) tc = 3;
                    else if (m >= 10) tc = 2;
                    else tc = 1;

                    i += tc;

                    while ((tc > 0) && (m != 0))
                    {
                        devbuffer[xi+tc] = '0' + (char)(m % 10);
                        m /= 10;
                        tc--;
                    }
                }

                devbuffer[i] = 0;
            }

            break;
        case dft_pipe:
            modex = DMNAMEDPIPE;
            break;
        case dft_socket:
            modex = DMSOCKET;
            break;
        case dft_file:
            break;
    }

    d9r_reply_stat (io, tag, 0, 0, qid, modex | c->mode, c->atime, c->mtime,
                    c->length, c->name, c->uid, c->gid, c->muid, ex);
}

static void Topen (struct d9r_io *io, int_16 tag, int_32 fid, int_8 mode)
{
    struct d9r_fid_metadata *md = d9r_fid_metadata (io, fid);
    struct dfs_node_common *c = md->aux;
    struct d9r_qid qid = { 0, 1, (int_64)(int_pointer)c };

    switch (c->type)
    {
        case dft_directory:
            qid.type = QTDIR;
            break;
        case dft_symlink:
            qid.type = QTLINK;
            break;
        default:
            break;
    }

    d9r_reply_open (io, tag, qid, 0x1000);
}

static void add_file_with_content
    (char *name, char *contents, struct dfs_directory *d_dir)
{
    struct dfs_file *d_ip4 =
        dfs_mk_file (d_dir, name, (char *)0, (int_8 *)contents, 6, (void *)0,
                     (void *)0, (void *)0);

    d_ip4->c.mode     = 0650;
    d_ip4->c.uid      = "dnsfs";
    d_ip4->c.gid      = "dnsfs";
}

static void dnsfs_address_lookup (char *name, struct dfs_directory *d_dir)
{
    struct addrinfo *ai;
    int r = getaddrinfo (name, (void *)0, (void *)0, &ai);

    if (r == 0)
    {
        struct addrinfo *c = ai;
	while (c->ai_next != (struct addrinfo *)0)
        {
	    switch (c->ai_family)
	    {
	        case AF_INET:
		    break;
	    }

	    c = c->ai_next;
	}
        add_file_with_content ("ip4", "(nop)\n", d_dir);
        freeaddrinfo (ai);
    }
}

static void Tcreate (struct d9r_io *io, int_16 tag, int_32 fid, char *name, int_32 perm, int_8 mode, char *ext)
{
    struct d9r_fid_metadata *md = d9r_fid_metadata (io, fid);
    struct dfs_node_common *c = md->aux;
    struct dfs_directory *d;
    struct d9r_qid qid = { 0, 1, 2 };

    if (c->type != dft_directory)
    {
        d9r_reply_error (io, tag,
                         "Cannot create nodes under anything but a directory.",
                         P9_EDONTCARE);
        return;
    }

    d = (struct dfs_directory *)c;

    if (perm & DMDIR)
    {
        struct dfs_directory *d_dir = dfs_mk_directory (d, name);

        d_dir->c.mode     = 0550;
        d_dir->c.uid      = "dnsfs";
        d_dir->c.gid      = "dnsfs";

	dnsfs_address_lookup (name, d_dir);

        qid.type = QTDIR;
        qid.path = (int_64)(int_pointer)d_dir;
    }
    else if (perm & DMSYMLINK)
    {
        qid.type = QTLINK;
        qid.path = (int_64)(int_pointer)dfs_mk_symlink(d, name, ext);
    }
    else if (perm & DMSOCKET)
    {
        qid.path = (int_64)(int_pointer)dfs_mk_socket(d, name);
    }
    else if (perm & DMNAMEDPIPE)
    {
        qid.path = (int_64)(int_pointer)dfs_mk_pipe(d, name);
    }
    else if (perm & DMDEVICE)
    {
        int_16 majour = 0, minor = 0, i = 1;

        if ((ext == (char *)0) || (ext[0] == (char)0) || (ext[1] == (char)0) ||
            ((ext[0] != 'c') && (ext[0] != 'b')))
        {
            d9r_reply_error (io, tag, "Invalid Tcreate Message.", P9_EDONTCARE);
            return;
        }

        while (ext[i] == ' ') i++;

        while (ext[i] && (ext[i] != ' '))
        {
            majour *= 10;
            majour += (char)(ext[i] - '0');
            i++;
        }

        while (ext[i] == ' ') i++;

        while (ext[i])
        {
            minor *= 10;
            minor += (char)(ext[i] - '0');
            i++;
        }

        qid.path = (int_64)(int_pointer)dfs_mk_device
                (d, name,
                 (ext[0] == 'b') ? dfs_block_device : dfs_character_device,
                  majour, minor);
    }
    else
    {
        qid.path = (int_64)(int_pointer)dfs_mk_file
                (d, name, (char *)0, (int_8 *)0, 0, (void *)0, (void *)0, (void *)0);
    }

    d9r_reply_create (io, tag, qid, 0x1000);
}

struct Tread_dir_map
{
    int_32 index;
    int_16 tag;
    char replied;
    struct d9r_io *io;
};

static void Tread_dir (struct tree_node *node, void *v)
{
    struct Tread_dir_map *m = (struct Tread_dir_map *)v;

    if (m->index == 0)
    {
        struct dfs_node_common *c
                = (struct dfs_node_common *)node_get_value (node);

        if (c != (struct dfs_node_common *)0)
        {
            int_8 *bb;
            int_16 slen = 0;
            int_32 modex = 0;
            struct d9r_qid qid = { 0, 1, (int_64)(int_pointer)c };

            switch (c->type)
            {
                case dft_directory:
                    qid.type = QTDIR;
                    modex = DMDIR;
                    break;
                case dft_symlink:
                    qid.type = QTLINK;
                    modex = DMSYMLINK;
                    break;
                case dft_device:
                    modex = DMDEVICE;
                    break;
                case dft_socket:
                    modex = DMSOCKET;
                    break;
                case dft_pipe:
                    modex = DMNAMEDPIPE;
                    break;
                case dft_file:
                    break;
            }

            slen = d9r_prepare_stat_buffer
                    (m->io, &bb, 0, 0, &qid, modex | c->mode, c->atime,
                     c->mtime, c->length, c->name, c->uid, c->gid, c->muid,
                     (char *)0);
            d9r_reply_read (m->io, m->tag, slen, bb);
            afree (slen, bb);

            m->replied = (char)1;
        }
    }

    m->index--;
}

static void Tread (struct d9r_io *io, int_16 tag, int_32 fid, int_64 offset, int_32 length)
{
    struct d9r_fid_metadata *md = d9r_fid_metadata (io, fid);
    struct dfs_node_common *c = md->aux;

    switch (c->type)
    {
        case dft_directory:
            {
                struct dfs_directory *dir = (struct dfs_directory *)c;
                if (offset == (int_64)0) md->index = 0;

                if (md->index == 0)
                {
                    int_8 *bb;
                    struct d9r_qid qid = { QTDIR, 1, (int_64)(int_pointer)c };
                    int_16 slen = d9r_prepare_stat_buffer
                            (io, &bb, 0, 0, &qid, DMDIR | dir->c.mode,
                             dir->c.atime, dir->c.mtime, dir->c.length, ".",
                             dir->c.uid, dir->c.gid, dir->c.muid, (char *)0);
                    d9r_reply_read (io, tag, slen, bb);
                    afree (slen, bb);
                }
                else if (md->index == 1)
                {
                    int_16 slen;
                    int_8 *bb;
                    struct d9r_qid qid = { QTDIR, 1, (int_64)(int_pointer)c };

                    dir = dir->parent;

                    slen = d9r_prepare_stat_buffer
                            (io, &bb, 0, 0, &qid, DMDIR | dir->c.mode,
                             dir->c.atime, dir->c.mtime, dir->c.length, "..",
                             dir->c.uid, dir->c.gid, dir->c.muid, (char *)0);
                    d9r_reply_read (io, tag, slen, bb);
                    afree (slen, bb);
                }
                else
                {
                    struct Tread_dir_map m
                            = { .tag = tag, .replied = (char)0, .io = io };

                    m.index = md->index - 2;

                    tree_map (dir->nodes, Tread_dir, (void *)&m);

                    if (m.replied == (char)0)
                    {
                        d9r_reply_read (io, tag, 0, (int_8 *)0);
                    }
                }

                (md->index)++;
            }
            break;
        case dft_file:
            {
                struct dfs_file *file = (struct dfs_file *)c;

                if (file->on_read == (void *)0)
                {
                    if (length > (file->c.length + offset))
                    {
                        length = file->c.length - offset;
                    }
                    d9r_reply_read (io, tag, length, (file->data + offset));
                }
                else
                {
                    file->on_read (io, tag, file, offset, length);
                }
            }
            break;
        default:
            d9r_reply_read (io, tag, 0, (int_8 *)0);
            break;
    }
}

static void Twrite (struct d9r_io *io, int_16 tag, int_32 fid, int_64 offset, int_32 count, int_8 *data)
{
    struct d9r_fid_metadata *md = d9r_fid_metadata (io, fid);
    struct dfs_node_common *c = md->aux;

    switch (c->type)
    {
        case dft_file:
            {
                struct dfs_file *f = (struct dfs_file *)c;

                if (f->on_write != (void *)0)
                {
                    d9r_reply_write
                            (io, tag, f->on_write (f, offset, count, data));
                    return;
                }
            }
            break;
        default:
            break;
    }

    d9r_reply_write (io, tag, count);
}

static void Twstat
        (struct d9r_io *io, int_16 tag, int_32 fid, int_16 type, int_32 dev,
         struct d9r_qid qid, int_32 mode, int_32 atime, int_32 mtime,
         int_64 length, char *name, char *uid, char *gid, char *muid, char *ex)
{
    d9r_reply_wstat(io, tag); /* stub reply with 'yes' */
}

static void Cclose (struct d9r_io *io)
{
    struct dfs *fs = (struct dfs *)io->aux;

    if (fs->close != (void *)0)
    {
        fs->close (io, fs->aux);
    }
}

static void initialise_io (struct d9r_io *io, struct dfs *fs)
{
    io->Tattach = Tattach;
    io->Twalk   = Twalk;
    io->Tstat   = Tstat;
    io->Topen   = Topen;
    io->Tcreate = Tcreate;
    io->Tread   = Tread;
    io->Twrite  = Twrite;
    io->Twstat  = Twstat;
    io->close   = Cclose;
    io->aux     = (void *)fs;

    multiplex_add_d9r (io, (void *)0);
}

static void multiplex_add_d9s_io_internal
    (struct io *, struct io *, struct dfs *);

static void on_connect(struct io *in, struct io *out, void *p) {
    multiplex_add_d9s_io_internal (in, out, (struct dfs *)p);
}

static void multiplex_d9s_internal ()
{
    static char initialised = 0;

    if (initialised == (char)0)
    {
        multiplex_network();
        multiplex_d9r();

        initialised = (char)1;
    }
}

static void multiplex_add_d9s_socket_internal (char *socketname, struct dfs *fs)
{
    multiplex_add_socket (socketname, on_connect, (void *)fs);
}

static void multiplex_add_d9s_io_internal (struct io *in, struct io *out, struct dfs *fs)
{
    struct d9r_io *io = d9r_open_io(in, out);

    if (io == (struct d9r_io *)0) return;

    initialise_io (io, fs);
}

static void multiplex_add_d9s_stdio_internal (struct dfs *fs)
{
    struct d9r_io *io = d9r_open_stdio();

    if (io == (struct d9r_io *)0) return;

    initialise_io (io, fs);
}

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

    multiplex_d9s_internal();

    if (use_stdio)
    {
        multiplex_add_d9s_stdio_internal (fs);
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
        multiplex_add_d9s_socket_internal (use_socket, fs);
    }

    while (multiplex() != mx_nothing_to_do);

    return 0;
}
