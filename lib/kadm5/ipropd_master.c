/*
 * Copyright (c) 1997, 1998 Kungliga Tekniska H�gskolan
 * (Royal Institute of Technology, Stockholm, Sweden). 
 * All rights reserved. 
 *
 * Redistribution and use in source and binary forms, with or without 
 * modification, are permitted provided that the following conditions 
 * are met: 
 *
 * 1. Redistributions of source code must retain the above copyright 
 *    notice, this list of conditions and the following disclaimer. 
 *
 * 2. Redistributions in binary form must reproduce the above copyright 
 *    notice, this list of conditions and the following disclaimer in the 
 *    documentation and/or other materials provided with the distribution. 
 *
 * 3. All advertising materials mentioning features or use of this software 
 *    must display the following acknowledgement: 
 *      This product includes software developed by Kungliga Tekniska 
 *      H�gskolan and its contributors. 
 *
 * 4. Neither the name of the Institute nor the names of its contributors 
 *    may be used to endorse or promote products derived from this software 
 *    without specific prior written permission. 
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND 
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE 
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE 
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE 
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL 
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS 
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) 
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT 
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY 
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF 
 * SUCH DAMAGE. 
 */

#include "kadm5_locl.h"

RCSID("$Id$");

static int
make_signal_socket (krb5_context context)
{
    struct sockaddr_un addr;
    int fd;

    fd = socket (AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0)
	krb5_err (context, 1, errno, "socket AF_UNIX");
    memset (&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy (addr.sun_path, KADM5_LOG_SIGNAL, sizeof(addr.sun_path));
    addr.sun_path[sizeof(addr.sun_path) - 1] = '\0';
    unlink (addr.sun_path);
    if (bind (fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	krb5_err (context, 1, errno, "bind %s", addr.sun_path);
    return fd;
}

static int
make_listen_socket (krb5_context context)
{
    int fd;
    int one = 1;
    struct sockaddr_in addr;

    fd = socket (AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
	krb5_err (context, 1, errno, "socket AF_INET");
    setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    memset (&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(4711);
    if(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	krb5_err (context, 1, errno, "bind");
    if (listen(fd, SOMAXCONN) < 0)
	krb5_err (context, 1, errno, "listen");
    return fd;
}

struct slave {
    int fd;
    struct sockaddr_in addr;
    char *name;
    krb5_auth_context ac;
    u_int32_t version;
    struct slave *next;
};

typedef struct slave slave;

static int
check_acl (krb5_context context, const char *name)
{
    FILE *fp;
    char buf[256];
    int ret = 1;

    fp = fopen (KADM5_SLAVE_ACL, "r");
    if (fp == NULL)
	return 1;
    while (fgets(buf, sizeof(buf), fp) != NULL) {
	if (buf[strlen(buf) - 1 ] == '\0')
	    buf[strlen(buf) - 1 ] = '\0';
	if (strcmp (buf, name) == 0) {
	    ret = 0;
	    break;
	}
    }
    fclose (fp);
    return ret;
}

static void
add_slave (krb5_context context, slave **root, int fd)
{
    krb5_principal server;
    krb5_error_code ret;
    slave *s;
    int addr_len;
    krb5_ticket *ticket = NULL;
    char hostname[128];

    s = malloc(sizeof(*s));
    if (s == NULL) {
	krb5_warnx (context, "add_slave: no memory");
	return;
    }
    s->name = NULL;
    s->ac = NULL;

    addr_len = sizeof(s->addr);
    s->fd = accept (fd, (struct sockaddr *)&s->addr, &addr_len);
    if (s->fd < 0) {
	krb5_warn (context, errno, "accept");
	goto error;
    }
    gethostname(hostname, sizeof(hostname));
    ret = krb5_sname_to_principal (context, hostname, IPROP_NAME,
				   KRB5_NT_SRV_HST, &server);
    if (ret) {
	krb5_warn (context, ret, "krb5_sname_to_principal");
	goto error;
    }

    ret = krb5_recvauth (context, &s->ac, &s->fd,
			 IPROP_VERSION, server, 0, NULL, &ticket);
    krb5_free_principal (context, server);
    if (ret) {
	krb5_warn (context, ret, "krb5_recvauth");
	goto error;
    }
    ret = krb5_unparse_name (context, ticket->client, &s->name);
    if (ret) {
	krb5_warn (context, ret, "krb5_unparse_name");
	goto error;
    }
    if (check_acl (context, s->name)) {
	krb5_warnx (context, "%s not in acl", s->name);
	goto error;
    }
    krb5_free_ticket (context, ticket);
    printf ("connection from %s\n", s->name);

    s->version = 0;
    s->next = *root;
    *root = s;
    return;
error:
    if (s->name)
	free (s->name);
    if (s->ac)
	krb5_auth_con_free(context, s->ac);
    if (ticket)
    krb5_free_ticket (context, ticket);
    close (s->fd);
    free(s);
}

int
main(int argc, char **argv)
{
    krb5_error_code ret;
    krb5_context context;
    void *kadm_handle;
    kadm5_server_context *server_context;
    kadm5_config_params conf;
    int signal_fd, listen_fd;
    int log_fd;
    slave *slaves = NULL;
    u_int32_t version, old_version = 0;

    set_progname(argv[0]);

    krb5_init_context(&context);

    memset(&conf, 0, sizeof(conf));
    ret = kadm5_s_init_with_password_ctx (context,
					  KADM5_ADMIN_SERVICE,
					  "password",
					  KADM5_ADMIN_SERVICE,
					  &conf, 0, 0, 
					  &kadm_handle);
    if (ret)
	krb5_err (context, 1, ret, "kadm5_s_init_with_password_ctx");

    server_context = (kadm5_server_context *)kadm_handle;

    log_fd = open (server_context->log_context.log_file, O_RDONLY, 0);
    if (log_fd < 0)
	krb5_err (context, 1, errno, "open %s",
		  server_context->log_context.log_file);

    signal_fd = make_signal_socket (context);
    listen_fd = make_listen_socket (context);

    for (;;) {
	slave *p;
	fd_set readset;
	int max_fd = 0;
	struct timeval to = {30, 0};
	u_int32_t vers;

	FD_ZERO(&readset);
	FD_SET(signal_fd, &readset);
	max_fd = max(max_fd, signal_fd);
	FD_SET(listen_fd, &readset);
	max_fd = max(max_fd, listen_fd);

	for (p = slaves; p != NULL; p = p->next) {
	    FD_SET(p->fd, &readset);
	    max_fd = max(max_fd, p->fd);
	}

	ret = select (max_fd + 1,
		      &readset, NULL, NULL, &to);
	if (ret < 0) {
	    if (errno == EINTR)
		continue;
	    else
		krb5_err (context, 1, errno, "select");
	}
	kadm5_log_get_version (log_fd, &version);
	if (version > old_version)
	    ; /* XXX - send out updates */
	if (ret && FD_ISSET(signal_fd, &readset)) {
	    struct sockaddr_un peer_addr;
	    size_t peer_len = sizeof(peer_addr);

	    if(recvfrom(signal_fd, &vers, sizeof(vers), 0,
			(struct sockaddr *)&peer_addr, &peer_len) < 0) {
		krb5_warn (context, errno, "recvfrom");
		continue;
	    }
	    printf ("signal: %u\n", vers);
	    --ret;
	}
	if (ret && FD_ISSET(listen_fd, &readset)) {
	    add_slave (context, &slaves, listen_fd);
	    --ret;
	}

	for(p = slaves; ret-- && p != NULL; p = p->next)
	    if (FD_ISSET(p->fd, &readset)) {
		/* XXX */
	    }
    }

    return 0;
}
