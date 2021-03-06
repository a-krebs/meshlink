#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "meshlink/meshlink.h"

volatile bool bar_reachable = false;

void status_cb(meshlink_handle_t *mesh, meshlink_node_t *node, bool reachable) {
	if(!strcmp(node->name, "bar"))
		bar_reachable = reachable;
}

int main(int argc, char *argv[]) {
	// Open two new meshlink instance.

	meshlink_handle_t *mesh1 = meshlink_open("import_export_conf.1", "foo", "import-export", DEV_CLASS_BACKBONE);
	if(!mesh1) {
		fprintf(stderr, "Could not initialize configuration for foo\n");
		return 1;
	}

	meshlink_handle_t *mesh2 = meshlink_open("import_export_conf.2", "bar", "import-export", DEV_CLASS_BACKBONE);
	if(!mesh2) {
		fprintf(stderr, "Could not initialize configuration for bar\n");
		return 1;
	}

	// Import and export both side's data

	meshlink_add_address(mesh1, "localhost");
	meshlink_add_address(mesh2, "localhost");

	char *data = meshlink_export(mesh1);
	if(!data) {
		fprintf(stderr, "Foo could not export its configuration\n");
		return 1;
	}

	if(!meshlink_import(mesh2, data)) {
		fprintf(stderr, "Bar could not import foo's configuration\n");
		return 1;
	}

	free(data);

	data = meshlink_export(mesh2);
	if(!data) {
		fprintf(stderr, "Bar could not export its configuration\n");
		return 1;
	}


	if(!meshlink_import(mesh1, data)) {
		fprintf(stderr, "Foo could not import bar's configuration\n");
		return 1;
	}

	free(data);

	// Start both instances

	meshlink_set_node_status_cb(mesh1, status_cb);
	
	if(!meshlink_start(mesh1)) {
		fprintf(stderr, "Foo could not start\n");
		return 1;
	}

	if(!meshlink_start(mesh2)) {
		fprintf(stderr, "Bar could not start\n");
		return 1;
	}

	// Wait for the two to connect.

	for(int i = 0; i < 20; i++) {
		sleep(1);
		if(bar_reachable)
			break;
	}

	if(!bar_reachable) {
		fprintf(stderr, "Bar not reachable for foo after 20 seconds\n");
		return 1;
	}

	int pmtu = meshlink_get_pmtu(mesh2, meshlink_get_node(mesh2, "bar"));
	for(int i = 0; i < 10 && !pmtu; i++) {
		sleep(1);
		pmtu = meshlink_get_pmtu(mesh2, meshlink_get_node(mesh2, "bar"));
	}

	if(!pmtu) {
		fprintf(stderr, "UDP communication with bar not possible after 10 seconds\n");
		return 1;
	}

	// Clean up.

	meshlink_stop(mesh2);
	meshlink_stop(mesh1);
	meshlink_close(mesh2);
	meshlink_close(mesh1);

	return 0;
}
