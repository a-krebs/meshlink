#include <cstring>
#include <iostream>
#include <unistd.h>
#include <cerrno>

#include "meshlink/meshlink++.h"

using namespace std;

int main(int argc, char *argv[]) {
	// Open a new meshlink instance.

	meshlink::mesh mesh;
	mesh.open("basicpp_conf", "foo", "basicpp", DEV_CLASS_BACKBONE);

	// Check that our own node exists.

	meshlink::node *self = mesh.get_self();
	if(!self) {
		cerr << "Foo does not know about itself\n";
		return 1;
	}
	if(strcmp(self->name, "foo")) {
		cerr << "Foo thinks its name is " << self->name << "\n";
		return 1;
	}

	// Start and stop the mesh.

	if(!mesh.start(MESHLINK_DEBUG)) {
		cerr << "Foo could not start\n";
		return 1;
	}
	mesh.stop();

	// Make sure we can start and stop the mesh again.

	if(!mesh.start(MESHLINK_DEBUG)) {
		cerr << "Foo could not start twice\n";
		return 1;
	}
	mesh.stop();

	// Close the mesh and open it again, now with a different name parameter.

	mesh.close();

	// Check that the name is ignored now, and that we still are "foo".

	if(mesh.open("basicpp_conf", "bar", "basicpp", DEV_CLASS_BACKBONE)) {
		cerr << "Could reopen configuration using name bar instead of foo\n";
		return 1;
	}

	if(!mesh.open("basicpp_conf", NULL, "basicpp", DEV_CLASS_BACKBONE)) {
		cerr << "Could not open configuration for foo a second time\n";
		return 1;
	}

	if(mesh.get_node("bar")) {
		cerr << "Foo knows about bar, it shouldn't\n";
		return 1;
	}

	self = mesh.get_self();
	if(!self) {
		cerr << "Foo doesn't know about itself the second time\n";
		return 1;
	}
	if(strcmp(self->name, "foo")) {
		cerr << "Foo thinks its name is " << self->name << " the second time\n";
		return 1;
	}

	// Start and stop the mesh.

	if(!mesh.start(MESHLINK_DEBUG)) {
		cerr << "Foo could not start a third time\n";
		return 1;
	}

	mesh.stop();

	if(!meshlink::destroy("basicpp_conf")) {
		cerr << "Could not destroy configuration\n";
		return 1;
	}

	if(!access("basic.conf", F_OK) || errno != ENOENT) {
		cerr << "Configuration not fully destroyed\n";
		return 1;
	}

	if(mesh.open("basicpp_conf", NULL, "basicpp", DEV_CLASS_BACKBONE)) {
		cerr << "Could open non-existing configuration with NULL name\n";
		return 1;
	}

	return 0;
}
