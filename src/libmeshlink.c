/*
    libmeshlink.h -- Tincd Library
    Copyright (C) 2014 Guus Sliepen <guus@meshlink.io> Saverio Proto <zioproto@gmail.com>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "system.h"

#include <pthread.h>

#include "connection.h"
#include "crypto.h"
#include "ecdsagen.h"
#include "edge.h"
#include "libmeshlink.h"
#include "net.h"
#include "node.h"
#include "protocol.h"

char *hosts_dir = NULL;
recvdata_cb_t recv_callback;
static char *name = NULL;
char *tinc_conf = NULL;
static bool tty = false;

#ifdef HAVE_MLOCKALL
/* If nonzero, disable swapping for this process. */
static bool do_mlock = false;
#endif

/* Open a file with the desired permissions, minus the umask.
   Also, if we want to create an executable file, we call fchmod()
   to set the executable bits. */

FILE *fopenmask(const char *filename, const char *mode, mode_t perms) {
	mode_t mask = umask(0);
	perms &= ~mask;
	umask(~perms);
	FILE *f = fopen(filename, mode);
#ifdef HAVE_FCHMOD
	if((perms & 0444) && f)
		fchmod(fileno(f), perms);
#endif
	umask(mask);
	return f;
}

static void disable_old_keys(const char *filename, const char *what) {
	char tmpfile[PATH_MAX] = "";
	char buf[1024];
	bool disabled = false;
	bool block = false;
	bool error = false;
	FILE *r, *w;

	r = fopen(filename, "r");
	if(!r)
		return;

	snprintf(tmpfile, sizeof tmpfile, "%s.tmp", filename);

	struct stat st = {.st_mode = 0600};
	fstat(fileno(r), &st);
	w = fopenmask(tmpfile, "w", st.st_mode);

	while(fgets(buf, sizeof buf, r)) {
		if(!block && !strncmp(buf, "-----BEGIN ", 11)) {
			if((strstr(buf, " EC ") && strstr(what, "ECDSA")) || (strstr(buf, " RSA ") && strstr(what, "RSA"))) {
				disabled = true;
				block = true;
			}
		}

		bool ecdsapubkey = !strncasecmp(buf, "ECDSAPublicKey", 14) && strchr(" \t=", buf[14]) && strstr(what, "ECDSA");

		if(ecdsapubkey)
			disabled = true;

		if(w) {
			if(block || ecdsapubkey)
				fputc('#', w);
			if(fputs(buf, w) < 0) {
				error = true;
				break;
			}
		}

		if(block && !strncmp(buf, "-----END ", 9))
			block = false;
	}

	if(w)
		if(fclose(w) < 0)
			error = true;
	if(ferror(r) || fclose(r) < 0)
		error = true;

	if(disabled) {
		if(!w || error) {
			fprintf(stderr, "Warning: old key(s) found, remove them by hand!\n");
			if(w)
				unlink(tmpfile);
			return;
		}

#ifdef HAVE_MINGW
		// We cannot atomically replace files on Windows.
		char bakfile[PATH_MAX] = "";
		snprintf(bakfile, sizeof bakfile, "%s.bak", filename);
		if(rename(filename, bakfile) || rename(tmpfile, filename)) {
			rename(bakfile, filename);
#else
		if(rename(tmpfile, filename)) {
#endif
			fprintf(stderr, "Warning: old key(s) found, remove them by hand!\n");
		} else  {
#ifdef HAVE_MINGW
			unlink(bakfile);
#endif
			fprintf(stderr, "Warning: old key(s) found and disabled.\n");
		}
	}

	unlink(tmpfile);
}

static FILE *ask_and_open(const char *filename, const char *what, const char *mode, bool ask, mode_t perms) {
	FILE *r;
	char *directory;
	char buf[PATH_MAX];
	char buf2[PATH_MAX];

	/* Check stdin and stdout */
	if(ask && tty) {
		/* Ask for a file and/or directory name. */
		fprintf(stderr, "Please enter a file to save %s to [%s]: ", what, filename);

		if(fgets(buf, sizeof buf, stdin) == NULL) {
			fprintf(stderr, "Error while reading stdin: %s\n", strerror(errno));
			return NULL;
		}

		size_t len = strlen(buf);
		if(len)
			buf[--len] = 0;

		if(len)
			filename = buf;
	}

#ifdef HAVE_MINGW
	if(filename[0] != '\\' && filename[0] != '/' && !strchr(filename, ':')) {
#else
	if(filename[0] != '/') {
#endif
		/* The directory is a relative path or a filename. */
		directory = get_current_dir_name();
		snprintf(buf2, sizeof buf2, "%s" SLASH "%s", directory, filename);
		filename = buf2;
	}

	disable_old_keys(filename, what);

	/* Open it first to keep the inode busy */

	r = fopenmask(filename, mode, perms);

	if(!r) {
		fprintf(stderr, "Error opening file `%s': %s\n", filename, strerror(errno));
		return NULL;
	}

	return r;
}

/*
  Generate a public/private ECDSA keypair, and ask for a file to store
  them in.
*/
bool ecdsa_keygen(bool ask) {
	ecdsa_t *key;
	FILE *f;
	char *pubname, *privname;

	fprintf(stderr, "Generating ECDSA keypair:\n");

	if(!(key = ecdsa_generate())) {
		fprintf(stderr, "Error during key generation!\n");
		return false;
	} else
		fprintf(stderr, "Done.\n");

	xasprintf(&privname, "%s" SLASH "ecdsa_key.priv", confbase);
	f = ask_and_open(privname, "private ECDSA key", "a", ask, 0600);
	free(privname);

	if(!f)
		return false;

	if(!ecdsa_write_pem_private_key(key, f)) {
		fprintf(stderr, "Error writing private key!\n");
		ecdsa_free(key);
		fclose(f);
		return false;
	}

	fclose(f);

	if(name)
		xasprintf(&pubname, "%s" SLASH "hosts" SLASH "%s", confbase, name);
	else
		xasprintf(&pubname, "%s" SLASH "ecdsa_key.pub", confbase);

	f = ask_and_open(pubname, "public ECDSA key", "a", ask, 0666);
	free(pubname);

	if(!f)
		return false;

	char *pubkey = ecdsa_get_base64_public_key(key);
	fprintf(f, "ECDSAPublicKey = %s\n", pubkey);
	free(pubkey);

	fclose(f);
	ecdsa_free(key);

	return true;
}

static bool try_bind(int port) {
	struct addrinfo *ai = NULL;
	struct addrinfo hint = {
		.ai_flags = AI_PASSIVE,
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM,
		.ai_protocol = IPPROTO_TCP,
	};

	char portstr[16];
	snprintf(portstr, sizeof portstr, "%d", port);

	if(getaddrinfo(NULL, portstr, &hint, &ai) || !ai)
		return false;

	while(ai) {
		int fd = socket(ai->ai_family, SOCK_STREAM, IPPROTO_TCP);
		if(!fd)
			return false;
		int result = bind(fd, ai->ai_addr, ai->ai_addrlen);
		closesocket(fd);
		if(result)
			return false;
		ai = ai->ai_next;
	}

	return true;
}

int check_port(const char *name) {
	if(try_bind(655))
		return 655;

	fprintf(stderr, "Warning: could not bind to port 655. ");

	for(int i = 0; i < 100; i++) {
		int port = 0x1000 + (rand() & 0x7fff);
		if(try_bind(port)) {
			char *filename;
			xasprintf(&filename, "%s" SLASH "hosts" SLASH "%s", confbase, name);
			FILE *f = fopen(filename, "a");
			free(filename);
			if(!f) {
				fprintf(stderr, "Please change tinc's Port manually.\n");
				return 0;
			}

			fprintf(f, "Port = %d\n", port);
			fclose(f);
			fprintf(stderr, "Tinc will instead listen on port %d.\n", port);
			return port;
		}
	}

	fprintf(stderr, "Please change tinc's Port manually.\n");
	return 0;
}
//tinc_setup() should basically do what cmd_init() from src/tincctl.c does, except it doesn't have to generate a tinc-up script.
bool tinc_setup(const char* confbaseapi, const char* name) {
	confbase = xstrdup(confbaseapi);
        xasprintf(&tinc_conf, "%s" SLASH "tinc.conf", confbase);
        xasprintf(&hosts_dir, "%s" SLASH "hosts", confbase);
	if(!access(tinc_conf, F_OK)) {
		fprintf(stderr, "Configuration file %s already exists!\n", tinc_conf);
		return false;
	}

	if(!check_id(name)) {
		fprintf(stderr, "Invalid Name! Only a-z, A-Z, 0-9 and _ are allowed characters.\n");
		return false;
	}

	if(mkdir(confbase, 0777) && errno != EEXIST) {
		fprintf(stderr, "Could not create directory %s: %s\n", confbase, strerror(errno));
		return false;
	}

	if(mkdir(hosts_dir, 0777) && errno != EEXIST) {
		fprintf(stderr, "Could not create directory %s: %s\n", hosts_dir, strerror(errno));
		return false;
	}

	FILE *f = fopen(tinc_conf, "w");
	if(!f) {
		fprintf(stderr, "Could not create file %s: %s\n", tinc_conf, strerror(errno));
		return 1;
	}

	fprintf(f, "Name = %s\n", name);
	fclose(f);

	if(!ecdsa_keygen(false))
		return false;

	check_port(name);

	return true;

}


bool tinc_start(const char* confbaseapi) {
	pthread_t tincThread;
	confbase = xstrdup(confbaseapi);
	pthread_create(&tincThread,NULL,tinc_main_thread,confbase);
	pthread_detach(tincThread);
return true;
}

__attribute__((constructor)) static void meshlink_init(void) {
	/* Slllluuuuuuurrrrp! */

	gettimeofday(&now, NULL);
	srand(now.tv_sec + now.tv_usec);
	crypto_init();
}

__attribute__((destructor)) static void meshlink_exit(void) {
	crypto_exit();
}


void *tinc_main_thread(void * in) {
	static bool status = false;
	/* If nonzero, write log entries to a separate file. */
	bool use_logfile = false;

	confbase = (char*) in;

	openlogger("tinc", LOGMODE_STDERR);

	init_configuration(&config_tree);

	if(!read_server_config())
		return false;

	//char *priority = NULL; //shoud be not needed in libmeshlink

	/* Setup sockets and open device. */

	if(!setup_network())
		goto end;

	/* Start main loop. It only exits when tinc is killed. */

	logger(DEBUG_ALWAYS, LOG_NOTICE, "Ready");

	try_outgoing_connections();

	status = main_loop();

	/* Shutdown properly. */

end:
	close_network_connections();

	logger(DEBUG_ALWAYS, LOG_NOTICE, "Terminating");

	//free(priority);

	exit_configuration(&config_tree);

	return (void *)status;
}

bool tinc_stop();

// can be called from any thread
bool tinc_send_packet(node_t *receiver, const char* buf, unsigned int len) {

	vpn_packet_t packet;
	tincpackethdr* hdr = (tincpackethdr *)packet.data;
	if (sizeof(tincpackethdr) + len > MAXSIZE) {

	//log something
	return false;
	}

	packet.probe = false;
	memset(hdr, 0, sizeof *hdr);
	memcpy(hdr->destination,receiver->name,sizeof(hdr->destination));
	memcpy(hdr->source,mesh->self->name,sizeof(hdr->source));

	packet.len = sizeof(tincpackethdr) + len;
	memcpy(packet.data+sizeof(tincpackethdr),buf,len);

        mesh->self->in_packets++;
        mesh->self->in_bytes += packet.len;
        route(mesh->self, &packet);

return true;
}

// handler runs in tinc thread and should return immediately
bool tinc_set_packet_receive_handler(void (*handler)(const char* sender, const char* buf, unsigned int len));


//It might also be a good idea to add the option of looking up hosts by public
//key (fingerprints) instead of names.

node_t *tinc_get_host(const char *name) {



};

bool tinc_get_hosts(node_t** hosts);

bool tinc_sign(const char* payload, unsigned int len, const char** signature);

int tinc_verify(const char* sender, const char* payload, unsigned int plen, const char* signature, unsigned int slen);

/*
TODO: It would be good to add a void pointer here that will be passed on to the
handler function whenever it is called, or have a void pointer in node_t
that can be filled in by the application. That way, you can easily link an
application-specific data structure to a node_t.
*/
void channel_set_packet_send_handler(int (*handler)(const char* receiver, const char* buf, unsigned int len));
void channel_packet_receive_handler(const char* sender, const char* buf, unsigned int len);

bool channel_open(const char* partner, void(*read)(int id, const char* buf, unsigned int len), void(*result)(int result, int id));
void channel_close(int id);
bool channel_write(int id, const char* buf, unsigned int len, void(*result)(int result, int id, unsigned int written));


//We do need some more functions. First of all, we need to be able to add nodes
//to a VPN. To do that, either an invitation protocol should be used:

bool tinc_join_network(const char *invitation);
const char *tinc_generate_invitation(const char *name);

/*
Or two nodes should exchange some information (their name, address, public
key). If the application provides a way to exchange some data with another
node, then:
*/

bool tinc_export(char *buf, size_t *len);
node_t *tinc_import(const char *buf, size_t len);
/*
Last but not least, some way to get rid of unwanted nodes. Simplest is a
function that just blacklists a node.
Which should immediately cause the local tincd to ignore any data from that
host from that point on. Of course, a somewhat centrally managed,
automatically distributed blacklist or whitelist would be the next step.
*/
bool tinc_blacklist(node_t *host);

void recvdata_register_cb(recvdata_cb_t cb) {
recv_callback = cb;
};


