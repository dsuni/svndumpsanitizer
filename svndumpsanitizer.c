/*
	svndumpsanitizer version 2.0.4, released 21 Aug 2017

	Copyright 2011,2012,2013,2014,2015,2016,2017 Daniel Suni

	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

// WIN32 modification by $ergi0, squeek502 and dufreyne
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#include <sys/types.h>
#include <io.h>
#include <fcntl.h>
#define fseeko _fseeki64
#endif

#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SDS_VERSION "2.0.4"
#define ADD 0
#define CHANGE 1
#define DELETE 2
#define REPLACE 3
#define CONTENT_PADDING 2
#define INCREMENT 10
#define NEWLINE 10

// For details on the svn dump file format see:
// http://svn.apache.org/repos/asf/subversion/trunk/notes/dump-load-format.txt

// A node from an svn dump file can contain a lot of data. This
// struct only holds the parts relevant to filtering.
typedef struct node {
	struct node **deps;
	char *path;
	char *copyfrom;
	int copyfrom_rev;
	int revision;
	unsigned short dep_len;
	char action;
	char wanted;
} node;

typedef struct {
	node *nodes;
	node **fakes;
	int size;
	int fake_size;
	int number;
} revision;

typedef struct repotree {
	struct repotree *children;
	char *path;
	node **map;
	unsigned short chi_len;
	unsigned short map_len;
} repotree;

typedef struct {
	char **path;
	int *from;
	int *to;
	unsigned short size;
} mergedata;

typedef struct {
	mergedata *data;
	int revision;
	int node;
	int orig_size;
} mergeinfo;

void exit_with_error(char *message, int exit_code) {
	fprintf(stderr, "ERROR: %s\n", message);
	exit(exit_code);
}

void show_help_and_exit() {
	printf("svndumpsanitizer usage:\n\n");
	printf("svndumpsanitizer [-i, --infile INFILE] [-o, --outfile OUTFILE] [OPTIONS]\n\n");
	printf("INFILE is mandatory; OUTFILE is optional. If omitted the output will be printed to\n");
	printf("stdout. Reading from stdin is not supported because svndumpsanitizer needs the data\n");
	printf("twice. (Once for analyzing, once for copying the selected parts.)\n\n");
	printf("OPTIONS\n");
	printf("\t-n, --include [PATHS]\n");
	printf("\t\tList of repository paths to include.\n\n");
	printf("\t-e, --exclude [PATHS]\n");
	printf("\t\tList of repository paths to exclude.\n\n");
	printf("\t\tYou must specify at least one path to include OR at least one path to exclude\n");
	printf("\t\tYou may not specify includes and excludes at the same time.\n\n");
	printf("\t\tPATHS are space separated paths as they appear in the svn repository\n");
	printf("\t\twithout leading or trailing slashes. E.g. \"-n trunk/foo/bar branches/baz\".\n\n");
	printf("\t\tThe only exception to this are directories in the repository root that start with a\n");
	printf("\t\thyphen. In order to not have this hyphen misinterpreted as a command, it must be\n");
	printf("\t\tescaped using a slash. E.g. \"-n /--foobar branches/baz\". Please notice that only\n");
	printf("\t\ta leading hyphen should be escaped, and only for the repository root.\n\n");
	printf("\t-d, --drop-empty\n");
	printf("\t\tAny revision that after sanitizing, contains no actions will be dropped altogether.\n");
	printf("\t\tThe remaining revisions will be renumbered. You will lose the commit messages for\n");
	printf("\t\tthe dropped revisions.\n\n");
	printf("\t-a, --add-delete\n");
	printf("\t\tAutomatically add a deleting revision to the end of the file that removes stuff that\n");
	printf("\t\tsvndumpsanitizer has been forced to keep due to dependencies, but actually resides in\n");
	printf("\t\tdirectories the user has specified he doesn't want. If no such files exist, no revision\n");
	printf("\t\twill be added. This was the default behavior prior to version 2.\n\n");
	printf("\t-r, --redefine-root [PATH]\n");
	printf("\t\tRedefines the repository root. This option can only be used with the include option.\n");
	printf("\t\tThe path provided to this option must be the beginning of (or the whole) path\n");
	printf("\t\tprovided to the include option. If more than one path is provided you can provide\n");
	printf("\t\ta path only up to the point where the paths diverge. The operation is not guaranteed\n");
	printf("\t\tto succeed on every repository. If the repository layout prevents redefining, a warning\n");
	printf("\t\twill be displayed, and the sanitizing will be performed without changing any paths.\n\n");
	printf("\t\tE.g.\n\t\t\"-n foo/bar/trunk -r foo/bar\" - OK. (This is probably the typical case.)\n");
	printf("\t\t\"-n foo/bar/trunk -r foo/bar/trunk\" - OK.\n");
	printf("\t\t\"-n foo/bar/trunk foo/baz/trunk -r foo\" - OK.\n");
	printf("\t\t\"-n foo/bar/trunk foo/baz/trunk -r foo/bar\" - WRONG.\n\n");
	printf("\t-q, --query\n");
	printf("\t\tOption used mostly for debugging purposes. If passed, svndumpsanitizer will after\n");
	printf("\t\treading and analyzing (but before writing) enter an interactive state where the user\n");
	printf("\t\tcan query the rationale why specific files are being included with the given options.\n");
	printf("\t\tThis query will locate the *first* dependency it can find that will include the file,\n");
	printf("\t\tnot all the dependencies, which might be numerous. When done, one can opt to either\n");
	printf("\t\tquit or proceed with writing the outfile.\n\n");
	printf("\t-v, --version\n");
	printf("\t\tPrint version and exit.\n");
	exit(0);
}

void show_version_and_exit() {
	printf("svndumpsanitizer %s\n", SDS_VERSION);
	exit(0);
}

/*******************************************************************************
 *
 * Misc helper functions
 *
 ******************************************************************************/

void print_progress(FILE *out, char *message, int progress) {
	fprintf(out, "%s %d\r", message, progress);
	fflush(out);
}

char* str_malloc(size_t sz) {
	char* str;
	if ((str = (char*)malloc(sz)) == NULL) {
		exit_with_error("malloc failed", 2);
	}
	return str;
}

char* add_slash_to(char *str) {
	char* new_str = str_malloc(strlen(str) + 2);
	strcpy(new_str, str);
	strcat(new_str, "/");
	return new_str;
}

// Returns 1 if string a starts with string b, otherwise 0
int starts_with(char *a, char *b) {
	int i = 0;
	while (b[i] != '\0') {
		if (a[i] != b[i]) {
			return 0;
		}
		++i;
	}
	return 1;
}

// Tries to match the beginning of a path to a name. Returns 1 if successful, otherwise 0.
// E.g. foo/bar/baz will match foo, or foo/bar but not foobar. 
int matches_path_start(char *path, char *name) {
	int i = 0;
	while (name[i] != '\0') {
		if (path[i] != name[i]) {
			return 0;
		}
		++i;
	}
	if (path[i] == '/' || path[i] == '\0') {
		return 1;
	}
	return 0;
}

// Reduces path as far as the redefined root allows. E.g. if foo/trunk is the redefined root
// foo/bar/baz.txt will be reduced to bar/baz.txt and foo/trunk/quux.txt will become quux.txt
char* reduce_path(char* redefined_root, char* path) {
	char* str;
	int i = 0;
	int mark = -1;
	str = str_malloc(strlen(path) + 1);
	while (redefined_root[i] != '\0') {
		if (path[i] != redefined_root[i]) {
			strcpy(str, &path[mark + 1]);
			// We've found a part of the redifined root, e.g. "trunk" when redefined_root is "trunk/foo"
			if (matches_path_start(redefined_root, str)) {
				str[0] = '\0';
			}
			return str;
		}
		if (path[i] == '/') {
			mark = i;
		}
		++i;
	}
	// This means we've found a path that can be fully reduced - i.e. the redefined root completely
	// matches the beginning of the path. E.g. redefined_root == foo/bar path == foo/bar/baz. i == 7 and
	// therefore points at the second slash. We return everything following that slash, but not the slash.
	if (path[i] == '/') {
		strcpy(str, &path[i + 1]);
		return str;
	}
	// This means that redefined_root == path. We return an empty string.
	else if (path[i] == '\0') {
		str[0] = '\0';
		return str;
	}
	// This means we've found a fake match. E.g. redefined_root == trunk/foo, path == trunk/foobar
	strcpy(str, &path[mark + 1]);
	return str;
}

// Returns the new file path after a copyfrom. E.g. New dir = branches/foo, old file is
// trunk/project/bar.txt branches/foo is copied from trunk. The method should return the
// location of the file after the copyfrom, i.e. branches/foo/project/bar.txt
char* get_dir_after_copyfrom(char *new, char *old, char *copyfrom) {
	char* temp_str = reduce_path(copyfrom, old);
	char* path = str_malloc(strlen(new) + strlen(temp_str) + 2);
	strcpy(path, new);
	strcat(path, "/");
	strcat(path, temp_str);
	free(temp_str);
	return path;
}

void init_new_node(node *n) {
	n[0].deps = NULL;
	n[0].path = NULL;
	n[0].copyfrom = NULL;
	n[0].copyfrom_rev = 0;
	n[0].revision = 0;
	n[0].action = 0;
	n[0].wanted = 0;
	n[0].dep_len = 0;
}

node* get_new_node() {
	node *n;
	if ((n = (node*)malloc(sizeof(node))) == NULL) {
		exit_with_error("malloc failed", 2);
	}
	init_new_node(n);
	return n;
}

// Cleans up memory reserved by tree (except root node's children)
void free_tree(repotree *rt) {
	int i;
	for (i = 0; i < rt->chi_len; ++i) {
		if (rt->children[i].chi_len > 0) {
			free_tree(&rt->children[i]);
		}
	}
	for (i = 0; i < rt->chi_len; ++i) {
		free(rt->children[i].path);
		free(rt->children[i].map);
		free(rt->children[i].children);
	}
}

// Returns the number of digits in an int
int num_len(int num) {
	int i = 0;
	do {
		num /= 10;
		++i;
	} while (num);
	return i;
}

// Returns the new revision number of a renumbered revision, OR the number of the first
// previous still included revision, should the revision in question have been dropped.
// Returns -1 if no such revision exists.
int get_new_revision_number(revision *revisions, int num) {
	int i = num;
	while (i > 0) {
		if (revisions[i].number > 0) {
			return revisions[i].number;
		}
		--i;
	}
	return -1;
}

// Returns 1 if the node is a fake, otherwise 0.
int is_node_fake(node *n, revision *revisions) {
	int i;
	for (i = 0; i < revisions[n->revision].fake_size; ++i) {
		if (revisions[n->revision].fakes[i] == n) {
			return 1;
		}
	}
	return 0;
}

/*******************************************************************************
 *
 * Include/exclude-related functions
 *
 ******************************************************************************/

// Is path [in/ex]cluded? paths_slash contain a copy of the paths with slashes
// appended to them. Yes, it could be done here, but this function is typically called
// from within nested for loops, and not building the path + slash over and over again
// will be much cheaper computationally.
int is_cluded(char *path, char **paths, char **paths_slash, int len) {
	int i;
	for (i = 0; i < len; ++i) {
		if (strcmp(path, paths[i]) == 0 || starts_with(path, paths_slash[i])) {
			return 1;
		}
	}
	return 0;
}

void print_why(node **nptr, char **inc, char **i_slash, char **exc, char **e_slash, char *why, int n_len, int i_len, int e_len) {
	int i, j;
	if (n_len < 0) {
		printf("Path %s does not seem to be included.\n", why);
		return;
	}
	for (i = n_len; i >= 0; --i) {
		printf("Revision %d: Path: %s  ", nptr[i]->revision, nptr[i]->path);
		for (j = 0; j < nptr[i]->dep_len; ++j) {
			if (nptr[i]->revision == nptr[i]->deps[j]->revision && nptr[i]->deps[j]->copyfrom) {
				printf(" (created by \"%s\" copyfrom \"%s\")", nptr[i]->deps[j]->path, nptr[i]->deps[j]->copyfrom);
				break;
			}
		}
		if (inc) {
			if (is_cluded(nptr[i]->path, inc, i_slash, i_len)) {
				printf(" PULLED BY INCLUDE");
			}
		}
		else if (!is_cluded(nptr[i]->path, exc, e_slash, e_len)) {
			printf(" NOT EXCLUDED");
		}
		if (i > 0) {
			printf(" depends on:");
		}
		printf("\n");
	}
}

// Dumb exclude that just marks excludes as unwanted. We'll have to come back
// later to check which ones are still needed due to dependencies
void parse_exclude_preparation(revision *r, char **excludes, char **ex_slash, int rev_len, int ex_len) {
	int i, j;
	for (i = 0; i < rev_len; ++i) {
		for (j = 0; j < r[i].size; ++j) {
			if (is_cluded(r[i].nodes[j].path, excludes, ex_slash, ex_len)) {
				r[i].nodes[j].wanted = 0;
			}
		}
		for (j = 0; j < r[i].fake_size; ++j) {
			if (is_cluded(r[i].fakes[j]->path, excludes, ex_slash, ex_len)) {
				r[i].fakes[j]->wanted = 0;
			}
		}
	}
}

// Sets the node and all its dependencies to "wanted"
void set_wanted(node *n) {
	int i;
	// Been here, done that...
	if (n->wanted == 1) {
		return;
	}
	n->wanted = 1;
	for (i = 0; i < n->dep_len; ++i) {
		set_wanted(n->deps[i]);
	}
}

// Returns the node that is relevant to the revision in question, or NULL if no such node exists.
node* get_node_at_revision(node **map, int rev, int map_len) {
	int i;
	for (i = map_len - 1; i >=0; --i) {
		if (map[i]->revision <= rev) {
			return map[i];
		}
	}
	return NULL;
}

// Returns the ADD node that is relevant to the revision in question, or NULL if no such node exists.
node* get_add_node_at_revision(node **map, int rev, int map_len) {
	int i;
	for (i = map_len - 1; i >=0; --i) {
		if (map[i]->revision <= rev) {
			if (map[i]->action == ADD) {
				return map[i];
			}
			if (map[i]->action == DELETE) {
				return NULL;
			}
		}
	}
	return NULL;
}

// Returns the node that is actually present at a certain revision when wanted status is considered.
node* get_wanted_node_at_revision(node **map, int rev, int map_len) {
	int i;
	for (i = map_len - 1; i >=0; --i) {
		if (map[i]->revision <= rev && map[i]->wanted) {
			if (map[i]->action == DELETE) {
				return NULL;
			}
			return map[i];
		}
	}
	return NULL;
}

node* get_node_at(node **map, int rev, int map_len, int wanted_only) {
	if (wanted_only) {
		return get_wanted_node_at_revision(map, rev, map_len);
	}
	return get_node_at_revision(map, rev, map_len);
}

/*******************************************************************************
 *
 * Repotree/dependency-related functions
 *
 ******************************************************************************/

// Returns subtree where the root node matches the provided path.
repotree* get_subtree(repotree *rt, char *path, int fail_if_not_found) {
	int i = 0;
	while (i < rt->chi_len) {
		if (matches_path_start(path, rt->children[i].path)) {
			if (strcmp(path, rt->children[i].path) == 0) {
				return &rt->children[i];
			}
			rt = &rt->children[i];
			i = -1;
		}
		++i;
	}
	// If it's the root dir, return entire tree.
	if (strlen(path) == 0) {
		return rt;
	}
	if (fail_if_not_found) {
		fprintf(stderr, "Could not find requested subtree: %s\n", path);
		exit_with_error("Internal logic error.", 3);
	}
	return NULL;
}

// This function tries to detect any colliding add or delete nodes when using redefine root.
// These situations can arise when something is copied from outside the included scope to a position
// where the redefine root operation will cause it to fall back to the original position, e.g.
// 1) Create foo/bar/baz.txt
// 2) Create trunk/project/foo/quux.txt
// 3) Copy bar (from foo/bar) to trunk/project/foo
// svndumpsanitizer ... -n trunk/project -r trunk/project => won't import due to double add of dir "foo"
int has_redefine_collisions(repotree *root, repotree *current, char *redefined_root) {
	int i, wanted;
	char *temp;
	node *n;
	repotree *subtree = NULL;
	if (current->path) {
		temp = reduce_path(redefined_root, current->path);
		if (strlen(temp) > 0 && strcmp(current->path, temp) != 0) {
			subtree = get_subtree(root, temp, 0);
		}
		free(temp);
		if (subtree) {
			wanted = 0; // We don't care about "collisions" that are unwanted, and won't be in the final repo.
			for (i = subtree->map_len - 1; i >= 0; --i) {
				if (subtree->map[i]->wanted) {
					wanted = 1;
				}
				// If the potential collision was deleted prior to the existence of the redefined path, it's not really a collision.
				if (subtree->map[i]->revision < current->map[0]->revision && subtree->map[i]->action == DELETE && subtree->map[i]->wanted) {
					break;
				}
				// Houston, we have a problem...
				if (wanted && (subtree->map[i]->action == DELETE || subtree->map[i]->action == ADD)) {
					return 1;
				}
			}
		}
	}
	for (i = 0; i < current->chi_len; ++i) {
		if (has_redefine_collisions(root, &current->children[i], redefined_root)) {
			return 1;
		}
	}
	return 0;
}

// Returns 1 if file (or dir) is still present after applying sanitazion rules, otherwise 0.
// no_search -> correct pointer is already given, skip searching for it.
int is_file_present(repotree *rt, revision *revisions, node *n, int no_search) {
	repotree *target = get_subtree(rt, n->path, 1);
	int i = target->map_len - 1;
	int j;
	char *temp;
	// Find the right pointer...
	if (!no_search) {
		while (i >= 0 && target->map[i] != n) {
			--i;
		}
		--i; // And back up to the previous one...
	}
	while (i >= 0 && target->map[i]->action != DELETE) {
		if (target->map[i]->wanted) {
			return 1;
		}
		// If it's a fake add node we need to dig deeper...
		if (target->map[i]->action == ADD && is_node_fake(target->map[i], revisions)) {
			// Is the dependency responsible for the fake wanted?
			if (target->map[i]->dep_len > 0 && target->map[i]->deps[0]->wanted && target->map[i]->deps[0]->copyfrom) {
				temp = add_slash_to(target->map[i]->deps[0]->path);
				if (starts_with(n->path, temp)) {
					free(temp);
					// Get the origin of the fake node. E.g. If the file trunk/project2/foo.txt has come to be
					// by copying trunk/project1 to trunk/project2, the origin would be trunk/project1/foo.txt.
					temp = get_dir_after_copyfrom(target->map[i]->deps[0]->copyfrom, n->path, target->map[i]->deps[0]->path);
					// Find the correct dependency
					for (j = 1; j < target->map[i]->dep_len; ++j) {
						if (strcmp(target->map[i]->deps[j]->path, temp) == 0) {
							// If it's wanted, we're done
							if (target->map[i]->deps[j]->wanted) {
								free(temp);
								return 1;
							}
							// If it's another fake add node, we need another go.
							else if (target->map[i]->deps[j]->action == ADD && is_node_fake(target->map[i]->deps[j], revisions)) {
								free(temp);
								return is_file_present(rt, revisions, target->map[i]->deps[j], 1);
							}
						}
					}
				}
				free(temp);
			}
		}
		--i;
	}
	return 0;
}

void restore_delete_node_if_needed(repotree *rt, revision *revisions, node *n) {
	if (is_file_present(rt, revisions, n, 0)) {
		n->wanted = 2;
	}
}

void add_dependency(node *master, node *slave) {
	if ((slave->deps = (node**)realloc(slave->deps, (slave->dep_len + 1) * sizeof(node*))) == NULL) {
		exit_with_error("realloc failed", 2);
	}
	slave->deps[slave->dep_len] = master;
	++slave->dep_len;
}

// Adds dependency to the relevant parent directory node. I.e. foo/bar/baz.txt depends
// on foo/bar, and foo/bar depends on foo. foo doesn't depend on anything.
void add_dir_dep_to_node(repotree *rt, node *n, int rev) {
	int i = strlen(n->path);
	char *path = str_malloc(i + 1);
	repotree *target;
	node *temp_n;
	strcpy(path, n->path);
	while (i && path[i] != '/') {
		--i;
	}
	// The node exists in root, and doesn't have this dependency.
	if (!i) {
		free(path);
		return;
	}
	path[i] = '\0';
	target = get_subtree(rt, path, 1);
	free(path);
	temp_n = get_add_node_at_revision(target->map, rev, target->map_len);
	if (temp_n && temp_n->action != DELETE) {
		add_dependency(temp_n, n);
	}
	else {
		fprintf(stderr, "Tried to add dependency to non-existing parent/revision. %d %s\n", rev, n->path);
		exit_with_error("Internal logic error", 3);
	}
}

// Adds dependency to previous version of file (which can actually be a dir).
// Only "ADD" nodes with copyfrom attribute should need this. All others should be
// handled immediately when the event is added.
void add_file_dep_to_node(repotree *rt, node *n) {
	repotree *target;
	node *temp_n;
	if (n->action != ADD || !n->copyfrom) {
		return;
	}
	target = get_subtree(rt, n->copyfrom, 1);
	// If we have the special case of the root directory being copied, we don't need any
	// additional dependencies since everything implicitly depends on the root dir anyway.
	if (target == rt) {
		return;
	}
	temp_n = get_node_at_revision(target->map, n->copyfrom_rev, target->map_len);
	if (temp_n && temp_n->action != DELETE) {
		add_dependency(temp_n, n);
	}
	else {
		fprintf(stderr, "Tried to add dependency to non-existing parent/revision. %d %d %d %s %s\n", n->revision, n->copyfrom_rev, n->action, n->path, n->copyfrom);
		exit_with_error("Internal logic error", 3);
	}
}

// If a file is merged into another we need to add a dependency...
void add_merge_dep_to_node(repotree *rt, node *n, char *mergefrom, char *mergeto, int rev) {
	repotree *subtree;
	node *temp_n;
	int alloc = 0;
	char *temp = add_slash_to(mergeto);
	// Check that file is actually relevant to merge before proceeding.
	if (!(starts_with(n->path, temp) || strcmp(n->path, mergeto) == 0)) {
		free(temp);
		return;
	}
	free(temp);
	if (strcmp(n->path, mergeto) == 0) {
		temp = mergefrom;
	}
	else {
		temp = get_dir_after_copyfrom(mergefrom, n->path, mergeto);
		alloc = 1;
	}
	// Get subtree without failing on error, because svn mergeinfo is an unholy mess.
	subtree = get_subtree(rt, temp, 0);
	if (alloc) {
		free(temp);
	}
	if (!subtree) {
		return;
	}
	temp_n = get_node_at_revision(subtree->map, rev, subtree->map_len);
	if (temp_n && temp_n->action != DELETE) {
		add_dependency(temp_n, n);
	}
}

int get_max_path_size(repotree *rt) {
	int i, current;
	int max = 0;
	if (rt->chi_len == 0) {
		return strlen(rt->path);
	}
	for (i = 0; i < rt->chi_len; ++i) {
		current = get_max_path_size(&rt->children[i]);
		if (current > max) {
			max = current;
		}
	}
	return max;
}

// Add event to repo tree. If the path in question does not yet exist, we add it to the tree.
// If it does exists we add it to the map. Dependencies are added for non-ADD-type nodes.
// ADD-types are either new files (=doesn't need this dependency) or copyfrom instances
// (=needs different dependecy, handled elsewhere).
void add_event(repotree *rt, node *n) { //, char *str, int rev_len) {
	int i;
	for (i = 0; i < rt->chi_len; ++i) {
		if (matches_path_start(n->path, rt->children[i].path)) {
			if (strcmp(n->path, rt->children[i].path) != 0) {
				add_event(&rt->children[i], n);
			}
			else {
				// Make everyone use the same pointer to save memory (important with huge 100GB+ repos)
				if (n->path != rt->children[i].path) {
					free(n->path);
					n->path = rt->children[i].path;
				}
				if (n->action != ADD) {
					add_dependency(rt->children[i].map[rt->children[i].map_len - 1], n);
				}
				if ((rt->children[i].map = (node**)realloc(rt->children[i].map, (rt->children[i].map_len + 1) * sizeof(node*))) == NULL) {
					exit_with_error("realloc failed", 2);
				}
				rt->children[i].map[rt->children[i].map_len] = n;
				++rt->children[i].map_len;
			}
			return;
		}
	}
	// New path - add it to tree.
	if ((rt->children = (repotree*)realloc(rt->children, (rt->chi_len + 1) * sizeof(repotree))) == NULL) {
		exit_with_error("realloc failed", 2);
	}
	rt->children[rt->chi_len].path = n->path;
	rt->children[rt->chi_len].children = NULL;
	rt->children[rt->chi_len].chi_len = 0;
	if ((rt->children[rt->chi_len].map = (node**)malloc(sizeof(node*))) == NULL) {
		exit_with_error("malloc failed", 2);
	}
	rt->children[rt->chi_len].map[0] = n;
	rt->children[rt->chi_len].map_len = 1;
	++rt->chi_len;
}

// Returns a list of node pointers present in a specific part of the tree at a specific revision.
// The number of nodes in the list will be "returned" through the "size" pointer.
node** get_relevant_nodes_at_revision(repotree *rt, int rev, int wanted_only, int *size) {
	int i, j, ch_size;
	int self_size = 0;
	node **nptr, **nptr2;
	node *n;
	for (i = 0; i < rt->chi_len; ++i) {
		n = get_node_at(rt->children[i].map, rev, rt->children[i].map_len, wanted_only);
		if (n && n->action != DELETE) {
			++self_size;
		}
	}
	if (!self_size) {
		*size = 0;
		return NULL;
	}
	if ((nptr = (node**)malloc(self_size * sizeof(node*))) == NULL) {
		exit_with_error("malloc failed", 2);
	}
	self_size = 0;
	for (i = 0; i < rt->chi_len; ++i) {
		n = get_node_at(rt->children[i].map, rev, rt->children[i].map_len, wanted_only);
		if (n && n->action != DELETE) {
			nptr[self_size] = n;
			++self_size;
		}
	}
	for (i = 0; i < rt->chi_len; ++i) {
		n = get_node_at(rt->children[i].map, rev, rt->children[i].map_len, wanted_only);
		if (n && n->action != DELETE) {
			nptr2 = get_relevant_nodes_at_revision(&rt->children[i], rev, wanted_only, &ch_size);
			if (nptr2) {
				if ((nptr = (node**)realloc(nptr, (ch_size + self_size) * sizeof(node*))) == NULL) {
					exit_with_error("realloc failed", 2);
				}
				for (j = 0; j < ch_size; ++j) {
					nptr[self_size] = nptr2[j];
					++self_size;
				}
				free(nptr2);
			}
		}
	}
	*size = self_size;
	return nptr;
}

/*******************************************************************************
 *
 * Mergeinfo-related functions
 *
 ******************************************************************************/

// Takes a string where newlines have been replaced with NULL chars.
// "Returns" the size of the data through the int pointer.
mergedata* add_mergedata(char *minfo, int *size) {
	mergedata *md;
	int i = 0;
	int len, j;
	if ((md = (mergedata*)malloc(sizeof(mergedata))) == NULL) {
		exit_with_error("malloc failed", 2);
	}
	md->size = 0;
	md->path = NULL;
	md->from = NULL;
	md->to = NULL;
	while (minfo[i] == '/') {
		len = strlen(&minfo[i]);
		if ((md->path = (char**)realloc(md->path, (md->size + 1) * sizeof(char*))) == NULL) {
			exit_with_error("realloc failed", 2);
		}
		if ((md->from = (int*)realloc(md->from, (md->size + 1) * sizeof(int))) == NULL) {
			exit_with_error("realloc failed", 2);
		}
		if ((md->to = (int*)realloc(md->to, (md->size + 1) * sizeof(int))) == NULL) {
			exit_with_error("realloc failed", 2);
		}
		j = i + len - 1;
		// Parse through possible crap at the end that isn't digits replacing it with NULL.
		while (minfo[j] < 48 || minfo[j] > 57) {
			minfo[j] = '\0';
			--j;
		}
		// Parse through the last number.
		while (minfo[j] >= 48 && minfo[j] <= 57) {
			--j;
		}
		md->to[md->size] = atoi(&minfo[j + 1]);
		// Parse until the first number, replacing all non-digits with NULL.
		while (minfo[j] != ':') {
			if (minfo[j] < 48 || minfo[j] > 57) {
				minfo[j] = '\0';
			}
			--j;
		}
		md->from[md->size] = atoi(&minfo[j + 1]);
		minfo[j] = '\0';
		md->path[md->size] = str_malloc(strlen(&minfo[i]));
		++i; // For some reason mergeinfo paths start with a slash, even though no other svn paths do.
		strcpy(md->path[md->size], &minfo[i]);
		i += len; // Step past the string and the NULL that terminates it.
		++md->size;
	}
	*size = i;
	return md;
}

mergeinfo* create_mergeinfo(mergeinfo *mi, char *minfo, int rev, int nod, int *mi_len) {
	int orig;
	int i = 0;
	// Parse past newlines turned NULL, and "K 13" line...
	while (minfo[i] == '\0' || minfo[i] == 'K' || minfo[i] == ' ' || minfo[i] == '1' || minfo[i] == '3') {
		++i;
	}
	// Mismatch here means it's not a mergeinfo. Abort.
	if (strcmp(&minfo[i], "svn:mergeinfo") != 0) {
		return mi;
	}
	i += 14; // 13 chars + NULL
	// Empty value == Abort. Why does svn even add this kind of manure to the dump file?
	if (strcmp(&minfo[i], "V 0") == 0) {
		return mi;
	}
	// Otherwise go past the "V (whatever)" line...
	while (minfo[i] != '\0') {
		++i;
	}
	++i;
	if ((mi = (mergeinfo*)realloc(mi, (*mi_len + 1) * sizeof(mergeinfo))) == NULL) {
		exit_with_error("realloc failed", 2);
	}
	mi[*mi_len].data = add_mergedata(&minfo[i], &orig);
	mi[*mi_len].revision = rev;
	mi[*mi_len].node = nod;
	mi[*mi_len].orig_size = i + orig + 9; // "PROPS-END"
	++*mi_len;
	return mi;
}

// Returns the number of bytes the final row will have, including newline.
int get_mergerow_size(mergedata *data, revision *revisions, char *redefined_root, int row) {
	int to, from;
	int size = 0;
	char *temp;
	to = get_new_revision_number(revisions, data->to[row]);
	from = get_new_revision_number(revisions, data->from[row]);
	if (to == from) {
		size += num_len(to) + 1; // ":XXX"
	}
	else {
		size += num_len(to) + num_len(from) + 2; // ":XXX-YYY"
	}
	if (redefined_root) {
		temp = reduce_path(redefined_root, data->path[row]);
		size += strlen(temp) + 2; // "/...\n"
		free(temp);
	}
	else {
		size += strlen(data->path[row]) + 2; // "/...\n"
	}
	return size;
}

void write_mergeinfo(FILE *outfile, mergedata *data, revision *revisions, char *redefined_root, int orig_size, off_t con_len, off_t pcon_len) {
	int i, v_size, diff, to, from;
	int size = 0;
	char *temp;
	for (i = 0; i < data->size; ++i) {
		size += get_mergerow_size(data, revisions, redefined_root, i);
	}
	v_size = size - 1; // The last (first?) newline doesn't count towards value length.
	size += num_len(v_size) + 3; // "V XXX\n"
	size += 30; // "\nK 13\nsvn:mergeinfo\n"  ... "\nPROPS-END"
	diff = orig_size - size;
	fprintf(outfile, "Prop-content-length: %d\nContent-length: %d\n\nK 13\nsvn:mergeinfo\nV %d\n",
					(int)pcon_len - diff, (int)con_len - diff, v_size);
	for (i = 0; i < data->size; ++i) {
		to = get_new_revision_number(revisions, data->to[i]);
		from = get_new_revision_number(revisions, data->from[i]);
		if (redefined_root) {
			temp = reduce_path(redefined_root, data->path[i]);
		}
		else {
			temp = data->path[i];
		}
		if (to == from) {
			fprintf(outfile, "/%s:%d\n", temp, to);
		}
		else {
			fprintf(outfile, "/%s:%d-%d\n", temp, from, to);
		}
		if (redefined_root) {
			free(temp);
		}
	}
}

/*******************************************************************************
 *
 * Main method
 *
 ******************************************************************************/

int main(int argc, char **argv) {
	// Misc temporary variables
 	int i, j, k, ch, want_by_default, new_number, empty, temp_int, is_dir, should_do, maxp_len;
	off_t offset, con_len, pcon_len;
	time_t rawtime;
	struct tm *ptm;
	char *temp_str = NULL;
	char *temp_str2 = NULL;
	char *minfo = NULL;
	int to_file = 1;
	int query = 0;
	int add_delete = 0;

	// Variables to help analyze user input 
	int in = 0;
	int out = 0;
	int incl = 0;
	int excl = 0;
	int drop = 0;
	int redef = 0;
	int del = 0;
	int why = 0;

	// Variables related to files and paths
	FILE *infile = NULL;
	FILE *outfile = NULL;
	FILE *messages = stdout;
	char **include = NULL; // Holds the paths the user wants to keep
	char **exclude = NULL; // Holds the paths the user wants to discard
	char **exc_slash = NULL;
	char **inc_slash = NULL;
	char **to_delete = NULL;
	char *redefined_root = NULL;
	char *why_file = NULL;
	node **redef_rollback = NULL;

	// Variables to hold the size of 2D pseudoarrays
	int inc_len = 0;
	int exc_len = 0;
	int del_len = 0;
	int cur_len = 0;
	int cur_max = 80;

	// File reading & writing variables
	char *current_line;
	if ((current_line = (char*)calloc(cur_max, 1)) == NULL) {
		exit_with_error("calloc failed", 2);
	}
	int reading_node = 0;
	int writing = 1;
	int toggle = 0;
	int merge = 0;

	// Variables related to revisions and nodes
	int drop_empty = 0;
	int rev_len = -1;
	int rev_max = 10;
	int rev = -1;
	revision *revisions;
	if ((revisions = (revision*)malloc(rev_max * sizeof(revision))) == NULL) {
		exit_with_error("malloc failed", 2);
	}
	int nod_len = -1;
	int nod = -1;
	node *current_node = NULL;
	node **node_ptr = NULL;

	// Create repotree root node
	repotree rt;
	rt.children = NULL;
	rt.path = NULL;
	rt.map = NULL;
	rt.chi_len = 0;
	repotree *subtree;

	mergeinfo *mi = NULL;
	int mi_max = 0;
	int mi_len = 0;
	int act_mi = -1;
	
	/*******************************************************************************
	 *
	 * Parameter analysis
	 *
	 *******************************************************************************/

	for (i = 1 ; i < argc ; ++i) {
		if (starts_with(argv[i], "-")) {
			if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
				free(current_line);
				free(revisions);
				free(include);
				free(exclude);
				show_help_and_exit();
			}
			if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
				free(current_line);
				free(revisions);
				free(include);
				free(exclude);
				show_version_and_exit();
			}
			in = (!strcmp(argv[i], "--infile") || !strcmp(argv[i], "-i"));
			out = (!strcmp(argv[i], "--outfile") || !strcmp(argv[i], "-o"));
			incl = (!strcmp(argv[i], "--include") || !strcmp(argv[i], "-n"));
			excl = (!strcmp(argv[i], "--exclude") || !strcmp(argv[i], "-e"));
			drop = (!strcmp(argv[i], "--drop-empty") || !strcmp(argv[i], "-d"));
			redef = (!strcmp(argv[i], "--redefine-root") || !strcmp(argv[i], "-r"));
			del = (!strcmp(argv[i], "--add-delete") || !strcmp(argv[i], "-a"));
			why = (!strcmp(argv[i], "--query") || !strcmp(argv[i], "-q"));
			if (!(in || out || incl || excl || drop || redef || del || why)) {
				exit_with_error(strcat(argv[i], " is not a valid parameter. Use -h for help."), 1);
			}
			else if (drop) {
				drop_empty = 1;
			}
			else if (del) {
				add_delete = 1;
			}
			else if (why) {
				query = 1;
			}
		}
		else if (in && infile == NULL) {
			infile = fopen(argv[i],"rb");
			if (infile == NULL) {
				exit_with_error(strcat(argv[i], " can not be opened as infile") , 3);
			}
		}
		else if (out && outfile == NULL) {
			outfile = fopen(argv[i],"wb");
			if (outfile == NULL) {
				exit_with_error(strcat(argv[i], " can not be opened as outfile") , 3);
			}
		}
		else if (incl) {
			if ((include = (char**)realloc(include, (inc_len + 1) * sizeof(char*))) == NULL) {
				exit_with_error("realloc failed", 2);
			}
			// Allow the user to escape a directory in the repository root, that starts with a
			// hyphen, using a slash.
			if (starts_with(argv[i], "/")) {
				include[inc_len] = &argv[i][1];
			}
			else {
				include[inc_len] = argv[i];
			}
			++inc_len;			
		}
		else if (excl) {
			if ((exclude = (char**)realloc(exclude, (exc_len + 1) * sizeof(char*))) == NULL) {
				exit_with_error("realloc failed", 2);
			}
			if (starts_with(argv[i], "/")) {
				exclude[exc_len] = &argv[i][1];
			}
			else {
				exclude[exc_len] = argv[i];
			}
			++exc_len;
		}
		else if (redef && redefined_root == NULL) {
			redefined_root = argv[i];
		}
		else {
			exit_with_error(strcat(argv[i], " is not a valid parameter. Use -h for help."), 1);
		}
	}
	if (infile == NULL) {
		exit_with_error("You must specify an infile", 1);
	}
	if (outfile == NULL) {
		to_file = 0;
		outfile = stdout;
// Without this output may be corrupted on windows.
#ifdef _WIN32
		_setmode(_fileno(outfile), _O_BINARY);
#endif
		messages = stderr;
	}
	if (include == NULL && exclude == NULL) {
		fclose(infile);
		if (to_file) {
			fclose(outfile);
		}
		exit_with_error("You must specify something to either include or exclude", 1);
	}
	if (include != NULL && exclude != NULL) {
		fclose(infile);
		if (to_file) {
			fclose(outfile);
		}
		exit_with_error("You may not specify both includes and excludes", 1);
	}
	if (exclude != NULL && redefined_root != NULL) {
		fclose(infile);
		if (to_file) {
			fclose(outfile);
		}
		exit_with_error("You may not redefine root when using excludes", 1);
	}
	if (redefined_root != NULL) {
		temp_str = add_slash_to(redefined_root);
		for (i = 0; i < inc_len; ++i) {
			if (!(strcmp(include[i], redefined_root) == 0 || starts_with(include[i], temp_str))) {
				fclose(infile);
				if (outfile != NULL) {
					fclose(outfile);
				}
				temp_str2 = str_malloc(strlen(redefined_root) + strlen(include[i]) + 43);
				strcpy(temp_str2, redefined_root);
				strcat(temp_str2, " can not be redefined as root for include ");
				strcat(temp_str2, include[i]);
				exit_with_error(temp_str2, 1);
			}
		}
		free(temp_str);
	}
	want_by_default = 10;
	if (include) {
		want_by_default = 0;
	}

	/*******************************************************************************
	 *
	 * Reading the metadata
	 *
	 *******************************************************************************/
	while ((ch = fgetc(infile)) != EOF) {
		// Once we reach a newline character we need to analyze the data.
		if (ch == NEWLINE) {
			// Data inside nodes needs special treatment.
			if (reading_node) {
				// An empty line while reading a node, means the node stops here.
				if (strlen(current_line) == 0) {
					reading_node = 0;
				}
				// A line starting with "Content-lenth: " means that the content
				// of the node is the only thing left of it.
				else if (starts_with(current_line, "Content-length: ")) {
					offset = (off_t)atol(&current_line[16]) + CONTENT_PADDING;
					if (is_dir && offset > 10 + CONTENT_PADDING) {
						minfo = str_malloc(offset + 1);
						i = 0;
						while ((ch = fgetc(infile)) != EOF && i < offset) {
							// NULL instead of newline means needed string operations are easier later.
							if (ch == NEWLINE) {
								minfo[i] = '\0';
							}
							else {
								minfo[i] = ch;
							}
							++i;
						}
						minfo[i] = '\0';
						mi = create_mergeinfo(mi, minfo, rev_len, nod_len, &mi_len);
						free(minfo);
					}
					else {
						fseeko(infile, offset, SEEK_CUR);
					}
					reading_node = 0;
				}
				else if (starts_with(current_line, "Node-action: ")) {
					if (strcmp(&current_line[13], "add") == 0) {
						current_node[nod_len].action = ADD;
					}
					else if (strcmp(&current_line[13], "delete") == 0) {
						current_node[nod_len].action = DELETE;
					}
					else if (strcmp(&current_line[13], "change") == 0) {
						current_node[nod_len].action = CHANGE;
					}
					else {
						current_node[nod_len].action = REPLACE;
					}
				}
				else if (starts_with(current_line, "Node-kind: ")) {
					is_dir = !strcmp(&current_line[11], "dir"); // Keep this info for now in case we need to analyze mergeinfo
				}
				else if (starts_with(current_line, "Node-copyfrom-path: ")) {
					current_node[nod_len].copyfrom = str_malloc(strlen(&current_line[19]));
					strcpy(current_node[nod_len].copyfrom, &current_line[20]);
				}
				else if (starts_with(current_line, "Node-copyfrom-rev: ")) {
					current_node[nod_len].copyfrom_rev = atoi(&current_line[19]);
				}
			} // End of "if (reading_node)"
			else if (starts_with(current_line, "Node-path: ")) {
				++nod_len;
				++revisions[rev_len].size;
				if (nod_len == 0) {
					if ((current_node = (node*)malloc(sizeof(node))) == NULL) {
						exit_with_error("malloc failed", 2);
					}
				}
				else if ((current_node = (node*)realloc(current_node, (nod_len + 1) * sizeof(node))) == NULL) {
					exit_with_error("realloc failed", 2);
				}
				init_new_node(&current_node[nod_len]);
				current_node[nod_len].path = str_malloc(strlen(&current_line[10]));
				strcpy(current_node[nod_len].path, &current_line[11]);
				current_node[nod_len].wanted = want_by_default;
				current_node[nod_len].revision = rev_len;
				reading_node = 1;
			}
			else if (starts_with(current_line,"Revision-number: ")) {
				if (rev_len >= 0) {
					revisions[rev_len].nodes = current_node;
				}
				++rev_len;
				print_progress(messages, "Reading revision", rev_len);
				if (rev_len == rev_max) {
					rev_max += INCREMENT;
					if ((revisions = (revision*)realloc(revisions, (rev_max * sizeof(revision)))) == NULL) {
						exit_with_error("realloc failed", 2);
					}
				}
				current_node = NULL;
				revisions[rev_len].nodes = NULL;
				revisions[rev_len].fakes = NULL;
				revisions[rev_len].size = 0;
				revisions[rev_len].fake_size = 0;
				revisions[rev_len].number = rev_len;
				nod_len = -1;
			}
			current_line[0] = '\0';
			cur_len = 0;
		} // End of "if (ch != NEWLINE)" 
		else {
			if (cur_len == cur_max - 1) {
				cur_max += INCREMENT;
				if ((current_line = (char*)realloc(current_line, cur_max)) == NULL) {
					exit_with_error("realloc failed", 2);
				}
			}
			current_line[cur_len] = ch;
			++cur_len;
			current_line[cur_len] = '\0';
		}
	 } // End of "while ((ch = fgetc(infile)) != EOF)"
	if (rev_len >= 0) {
		revisions[rev_len].nodes = current_node;
	}
	++rev_len;
	current_line[0] = '\0';
	cur_len = 0;
	fprintf(messages, "\n");

	/***********************************************************************************
	 *
	 * Create dependencies
	 *
	 ***********************************************************************************/

	if (mi_len > 0) {
		act_mi = 0;
	}
	
	for (i = 0; i < rev_len; ++i) {
		print_progress(messages, "Analyzing revision", i);
		merge = -1;
		for (j = 0; j < revisions[i].size; ++j) {
			add_event(&rt, &revisions[i].nodes[j]);
			// Some add nodes need this...
			if (revisions[i].nodes[j].action == ADD) {
				add_dir_dep_to_node(&rt, &revisions[i].nodes[j], i);
				add_file_dep_to_node(&rt, &revisions[i].nodes[j]);
			}
			if (act_mi >= 0 && mi[act_mi].revision == i && mi[act_mi].node == j) {
				merge = act_mi;
				++act_mi;
				if (act_mi == mi_len) {
					act_mi = -1;
				}
			}
			if (merge >= 0) {
				for (k = 0; k < mi[merge].data->size; ++k) {
					add_merge_dep_to_node(&rt, &revisions[i].nodes[j], mi[merge].data->path[k], revisions[i].nodes[mi[merge].node].path, mi[merge].data->to[k]);
				}
			}
			// Copyfrom and delete events can affect entire subtrees. This is dealt with here.
			if (revisions[i].nodes[j].copyfrom || revisions[i].nodes[j].action == DELETE) {
				if (revisions[i].nodes[j].copyfrom) {
					subtree = get_subtree(&rt, revisions[i].nodes[j].copyfrom, 1);
					node_ptr = get_relevant_nodes_at_revision(subtree, revisions[i].nodes[j].copyfrom_rev, 0, &temp_int);
				}
				else {
					subtree = get_subtree(&rt, revisions[i].nodes[j].path, 1);
					node_ptr = get_relevant_nodes_at_revision(subtree, i, 0, &temp_int);
				}
				if (temp_int == 0) {
					continue;
				}
				if ((revisions[i].fakes = (node**)realloc(revisions[i].fakes, (revisions[i].fake_size + temp_int) * sizeof(node*))) == NULL) {
					exit_with_error("realloc failed", 2);
				}
				for (k = 0; k < temp_int; ++k) {
					if ((current_node = (node*)malloc(sizeof(node))) == NULL) {
						exit_with_error("malloc failed", 2);
					}
					init_new_node(current_node);
					current_node->revision = i;
					current_node->action = revisions[i].nodes[j].action;
					current_node->wanted = want_by_default;
					if (revisions[i].nodes[j].copyfrom) {
						current_node->copyfrom = node_ptr[k]->path;
						current_node->copyfrom_rev = node_ptr[k]->revision;
						current_node->path = get_dir_after_copyfrom(revisions[i].nodes[j].path, node_ptr[k]->path, revisions[i].nodes[j].copyfrom);
					}
					else {
						current_node->path = node_ptr[k]->path;
					}
					add_event(&rt, current_node);
					add_dependency(&revisions[i].nodes[j], current_node); // Dependency on the node that affects the subtree
					if (revisions[i].nodes[j].copyfrom) {
						add_dependency(node_ptr[k], current_node); // File dependency
						add_dir_dep_to_node(&rt, current_node, i); // Dir dependency
					}
					revisions[i].fakes[revisions[i].fake_size] = current_node;
					++revisions[i].fake_size;
				}
				free(node_ptr);
			}
		}
	}

	/***********************************************************************************
	 *
	 * Analyze what to keep
	 *
	 ***********************************************************************************/

	// Include strategy
	if (include) {
		if ((inc_slash = (char**)malloc(inc_len * sizeof(char*))) == NULL) {
			exit_with_error("malloc failed", 2);
		}
		for (i = 0; i < inc_len; ++i) {
			inc_slash[i] = add_slash_to(include[i]);
		}
		for (i = rev_len - 1; i > 0; --i) {
			for (j = 0; j < revisions[i].size; ++j) {
				if (is_cluded(revisions[i].nodes[j].path, include, inc_slash, inc_len)) {
					set_wanted(&revisions[i].nodes[j]);
				}
			}
			for (j = 0; j < revisions[i].fake_size; ++j) {
				if (is_cluded(revisions[i].fakes[j]->path, include, inc_slash, inc_len)) {
					set_wanted(revisions[i].fakes[j]);
				}
			}
		}
	}
	// Exclude strategy
	else {
		if ((exc_slash = (char**)malloc(exc_len * sizeof(char*))) == NULL) {
			exit_with_error("malloc failed", 2);
		}
		for (i = 0; i < exc_len; ++i) {
			exc_slash[i] = add_slash_to(exclude[i]);
		}
		parse_exclude_preparation(revisions, exclude, exc_slash, rev_len, exc_len);
		for (i = rev_len - 1; i > 0; --i) {
			for (j = 0; j < revisions[i].size; ++j) {
				if (!is_cluded(revisions[i].nodes[j].path, exclude, exc_slash, exc_len)) {
					set_wanted(&revisions[i].nodes[j]);
				}
			}
		}
		for (i = rev_len - 1; i > 0; --i) {
			for (j = 0; j < revisions[i].fake_size; ++j) {
				if (!is_cluded(revisions[i].fakes[j]->path, exclude, exc_slash, exc_len)) {
					set_wanted(revisions[i].fakes[j]);
				}
			}
		}
	}

	/***********************************************************************************
	 *
	 * If user wants to question the reason for files being included, we do that now.
	 *
	 ***********************************************************************************/
	
	if (query) {
		maxp_len = get_max_path_size(&rt) + 10;
		why_file = str_malloc(maxp_len);
		do {
			printf("\nPlease enter the full (case sensitive) path name you wish to inquire about\n\"/quit\" will exit program, and \"/write\" proceed with writing the outfile:\n");
			fgets (why_file, maxp_len, stdin);
			i = 0;
			while (why_file[i] != NEWLINE && why_file[i] != '\0') {
				++i;
			}
			why_file[i] = '\0';
			if (strcmp(why_file, "/quit") == 0) {
				free(why_file);
				goto cleanup;
			}
			if (strcmp(why_file, "/write") == 0) {
				free(why_file);
				break;
			}
			node_ptr = NULL;
			temp_int = -1;
			for (i = 0; i < rev_len; ++i) {
				for (j = 0; j < revisions[i].size; ++j) {
					if (revisions[i].nodes[j].wanted) {
						if (!node_ptr) {
							if (strcmp(revisions[i].nodes[j].path, why_file) == 0) {
								if ((node_ptr = (node**)malloc(sizeof(node*))) == NULL) {
									exit_with_error("malloc failed", 2);
								}
								++temp_int;
								node_ptr[temp_int] = &revisions[i].nodes[j];
							}
						}
						else {
							for (k = 0; k < revisions[i].nodes[j].dep_len; ++k) {
								if (revisions[i].nodes[j].deps[k] == node_ptr[temp_int]) {
									++temp_int;
									if ((node_ptr = (node**)realloc(node_ptr, (temp_int + 1) * sizeof(node*))) == NULL) {
										exit_with_error("realloc failed", 2);
									}
									node_ptr[temp_int] = &revisions[i].nodes[j];
									break;
								}
							}
						}
					}
				}
				for (j = 0; j < revisions[i].fake_size; ++j) {
					if (revisions[i].fakes[j]->wanted) {
						for (k = 0; k < revisions[i].fakes[j]->dep_len; ++k) {
							if (node_ptr && revisions[i].fakes[j]->deps[k] == node_ptr[temp_int]) {
								++temp_int;
								if ((node_ptr = (node**)realloc(node_ptr, (temp_int + 1) * sizeof(node*))) == NULL) {
									exit_with_error("realloc failed", 2);
								}
								node_ptr[temp_int] = revisions[i].fakes[j];
								break;
							}
						}
					}
				}
			}
			print_why(node_ptr, include, inc_slash, exclude, exc_slash, why_file, temp_int, inc_len, exc_len);
			free(node_ptr);
		}	while (1);
	}
	
	// Restore wanted delete nodes
	for (i = 0; i < rev_len; ++i) {
		for (j = 0; j < revisions[i].size; ++j) {
			if (revisions[i].nodes[j].action == DELETE && !revisions[i].nodes[j].wanted) {
				restore_delete_node_if_needed(&rt, revisions, &revisions[i].nodes[j]);
			}
		}
	}

	// Renumber the revisions if the empty ones are to be dropped
	if (drop_empty) {
		revisions[0].number = 0; // Revision 0 is special, and should never be dropped.
		new_number = 1;
		for (i = 1; i < rev_len; ++i) {
			empty = 1;
			for (j = 0; j < revisions[i].size; ++j) {
				if (revisions[i].nodes[j].wanted) {
					empty = 0;
					break;
				}
			}
			if (empty) {
				revisions[i].number = -1;
			}
			else {
				revisions[i].number = new_number;
				++new_number;
			}
		}
	}
	fprintf(messages, "\n");

	// Remove any directory entries that should no longer exist with the redefined root
	if (redefined_root) {
		// First check whether the redefining even has a chance to succeed. If we have a redefined root of
		// "trunk/foo", and then try to do a copyfrom operation from "trunk", we're pretty much doomed...
		for (i = 0; i < rev_len; ++i) {
			for (j = 0; j < revisions[i].size; ++j) {
				if (revisions[i].nodes[j].copyfrom && revisions[i].nodes[j].wanted) {
					temp_str = reduce_path(redefined_root, revisions[i].nodes[j].copyfrom);
					if (strcmp(temp_str, "") == 0 && strcmp(redefined_root, revisions[i].nodes[j].copyfrom) != 0) {
						fprintf(stderr, "WARNING: Critical files detected upstream of redefined root.\n         Redefine operation will not be performed.\n");
						redefined_root = NULL;
						free(temp_str);
						goto write_out;
					}
					free(temp_str);
				}
			}
		}
		temp_int = 0;
		for (i = rev_len - 1; i >= 0; --i) {
			for (j = revisions[i].size - 1; j >= 0; --j) {
				if (revisions[i].nodes[j].wanted) {
					temp_str = add_slash_to(redefined_root);
					for (k = strlen(temp_str) - 1; k > 0; --k) {
						if (temp_str[k] == '/') {
							temp_str[k] = '\0';
							if (strcmp(temp_str, revisions[i].nodes[j].path) == 0) {
								if (revisions[i].nodes[j].wanted) {
									if ((redef_rollback = (node**)realloc(redef_rollback, (temp_int + 1) * sizeof(node*))) == NULL) {
										exit_with_error("realloc failed", 2);
									}
									redef_rollback[temp_int] = &revisions[i].nodes[j];
									++temp_int;
								}
								revisions[i].nodes[j].wanted = 0;
							}
						}
					}
					free(temp_str);
				}
			}
		}
		// Check that there are no collisions, roll back if there are.
		if (has_redefine_collisions(&rt, &rt, redefined_root)) {
			fprintf(stderr, "WARNING: File collisions detected with redefined root.\n         Redefine operation will not be performed.\n");
			redefined_root = NULL;
			for (i = 0; i < temp_int; ++i) {
				redef_rollback[i]->wanted = 1;
			}
		}
		free(redef_rollback);
	}
	
	/***********************************************************************************
	 *
	 * Write the outfile
	 *
	 ***********************************************************************************/
 write_out:
	if (mi_len > 0) {
		act_mi = 0;
	}
	reading_node = 0;
	fseeko(infile, 0, SEEK_SET); // Replaced "rewind(infile);" with this because (windows + huge files + rewind) == fail.
	while ((ch = fgetc(infile)) != EOF) {
		if (ch == NEWLINE) {
			if (reading_node) {
				if (strlen(current_line) == 0) {
					reading_node = 0;
					writing = 1;
				}
				else if (drop_empty && writing && starts_with(current_line, "Node-copyfrom-rev: ")) {
					temp_int = atoi(&current_line[19]);
					// It's possible for the copyfrom-rev argument to point to a revision that is being removed.
					// If this is the case we change it to point to the first revision prior to it, that remains.
					while (revisions[temp_int].number < 0) {
						--temp_int;
					}
					fprintf(outfile, "Node-copyfrom-rev: %d\n", revisions[temp_int].number);
					toggle = 1;
				}
				else if(writing && redefined_root && starts_with(current_line, "Node-copyfrom-path: ")) {
					temp_str = reduce_path(redefined_root, &current_line[20]);
					fprintf(outfile, "Node-copyfrom-path: %s\n", temp_str);
					toggle = 1;
					free(temp_str);	
				}
				else if (act_mi >= 0 && mi[act_mi].revision == rev && mi[act_mi].node == nod && starts_with(current_line, "Prop-content-length: ")) {
					pcon_len = (off_t)atol(&current_line[21]);
					toggle = 1;
				}
				else if (starts_with(current_line, "Content-length: ")) {
					con_len = (off_t)atol(&current_line[16]);
					if (writing) {
						if (pcon_len) {
							write_mergeinfo(outfile, mi[act_mi].data, revisions, redefined_root, mi[act_mi].orig_size, con_len, pcon_len);
							// -10 is because of the "PROPS-END\n" that is included in orig_size.
							fseeko(infile, mi[act_mi].orig_size - 10, SEEK_CUR);
							while (fgetc(infile) != NEWLINE) {}
							++act_mi;
							if (act_mi == mi_len) {
								act_mi = -1;
							}
						}
						else {
							fprintf(outfile, "%s\n", current_line);
						}
						for (offset = pcon_len; offset < con_len + CONTENT_PADDING; ++offset) {
							fputc(fgetc(infile), outfile);
						}
					}
					else {
						fseeko(infile, con_len + CONTENT_PADDING, SEEK_CUR);
					}
					reading_node = 0;
					writing = 1;
					toggle = 1;
				}
			}
			else if (starts_with(current_line, "Node-path: ")) {
				reading_node = 1;
				++nod;
				while (act_mi >= 0 && rev == mi[act_mi].revision && nod > mi[act_mi].node) {
					++act_mi;
					if (act_mi == mi_len) {
						act_mi = -1;
					}
				}
				pcon_len = 0;
				writing = revisions[rev].nodes[nod].wanted;
				if (writing && redefined_root != NULL) {
					temp_str = reduce_path(redefined_root, &current_line[11]);
					fprintf(outfile, "Node-path: %s\n", temp_str);
					toggle = 1;
					free(temp_str);
				}
			}
			else if (starts_with(current_line, "Revision-number: ")) {
				++rev;
				print_progress(messages, "Writing revision", rev);
				while (act_mi >= 0 && rev > mi[act_mi].revision) {
					++act_mi;
					if (act_mi == mi_len) {
						act_mi = -1;
					}
				}
				nod = -1;
				writing = (!drop_empty || revisions[rev].number >= 0);
				if (drop_empty && writing) {
					temp_int = atoi(&current_line[17]);
					fprintf(outfile, "Revision-number: %d\n", revisions[temp_int].number);
					toggle = 1;
				}
			}
			if (writing && !toggle) {
				// Had to replace fprintf(outfile, "%s\n", current_line); with this,
				// because, apparently it's possible to have NULL characters in SVN
				// commit messages... (WTF?)
				fwrite(current_line, 1, cur_len, outfile);
				fputc('\n', outfile);
			}
			else {
				toggle = 0;
			}
			current_line[0] = '\0';
			cur_len = 0;
		}
		else {
			current_line[cur_len] = ch;
			++cur_len;
			current_line[cur_len] = '\0';
		}
	}

	/***********************************************************************************
	 *
	 * Add deleting revision if wanted.
	 *
	 ***********************************************************************************/

	if (add_delete) {
		node_ptr = get_relevant_nodes_at_revision(&rt, rev_len, 1, &temp_int);
		if (include) {
			for (i = 0; i < temp_int; ++i) {
				should_do = 1;
				if (!is_cluded(node_ptr[i]->path, include, inc_slash, inc_len)) {
					temp_str = add_slash_to(node_ptr[i]->path);
					for (j = 0; j < inc_len; ++j) {
						if (starts_with(include[j], temp_str)) {
							should_do = 0;
							break;
						}
					}
					free(temp_str);
					if (!should_do) {
						continue;
					}
					for (j = 0; j < del_len; ++j) {
						temp_str = add_slash_to(to_delete[j]);
						if (starts_with(node_ptr[i]->path, temp_str)) {
							free(temp_str);
							should_do = 0;
							break;
						}
						free(temp_str);
					}
					if (should_do) {
						if ((to_delete = (char**)realloc(to_delete, (del_len + 1) * sizeof(char*))) == NULL) {
							exit_with_error("realloc failed", 2);
						}
						to_delete[del_len] = node_ptr[i]->path;
						++del_len;
					}
				}
			}
		}
		else {
			for (i = 0; i < temp_int; ++i) {
				should_do = 1;
				if (is_cluded(node_ptr[i]->path, exclude, exc_slash, exc_len)) {
					for (j = 0; j < del_len; ++j) {
						temp_str = add_slash_to(to_delete[j]);
						if (starts_with(node_ptr[i]->path, temp_str)) {
							free(temp_str);
							should_do = 0;
							break;
						}
						free(temp_str);
					}
					if (should_do) {
						if ((to_delete = (char**)realloc(to_delete, (del_len + 1) * sizeof(char*))) == NULL) {
							exit_with_error("realloc failed", 2);
						}
						to_delete[del_len] = node_ptr[i]->path;
						++del_len;
					}
				}
			}
		}
		free(node_ptr);
	}

	if (del_len > 0) {
		time(&rawtime);
		ptm = gmtime(&rawtime);
		if (drop_empty) {
			i = 1;
			do {
				temp_int = revisions[rev_len - i].number + 1;
				++i;
			} while (temp_int == 0);
		}
		else {
			temp_int = rev_len;
		}
		fprintf(outfile, "Revision-number: %d\n", temp_int);
		fprintf(outfile, "Prop-content-length: 133\n");
		fprintf(outfile, "Content-length: 133\n\n");
		fprintf(outfile, "K 7\nsvn:log\nV 22\n");
		fprintf(outfile, "Deleted unwanted nodes\n");
		fprintf(outfile, "K 10\nsvn:author\nV 16\nsvndumpsanitizer\nK 8\nsvn:date\nV 27\n");
		fprintf(outfile, "%d-%.2d-%.2dT%.2d:%.2d:%.2d.000000Z\n", ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday, ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
		fprintf(outfile, "PROPS-END\n\n");
		for (i = 0; i < del_len; ++i) {
			fprintf(outfile, "Node-path: %s\n", to_delete[i]);
			fprintf(outfile, "Node-action: delete\n\n\n");
		}
	}
	
	fprintf(messages, "\nAll done.\n");
	// Clean everything up
 cleanup:
	fclose(infile);
	if (to_file) {
		fclose(outfile);
	}
	for (i = 0; i < rev_len; ++i) {
		for (j = 0; j < revisions[i].size; ++j) {
			free(revisions[i].nodes[j].copyfrom);
			free(revisions[i].nodes[j].deps);
		}
		for (j = 0; j < revisions[i].fake_size; ++j) {
			free(revisions[i].fakes[j]->deps);
			free(revisions[i].fakes[j]);
		}
		free(revisions[i].nodes);
		free(revisions[i].fakes);
	}
	for (i = 0; i < mi_len; ++i) {
		for (j = 0; j < mi[i].data->size; ++j) {
			free(mi[i].data->path[j]);
		}
		free(mi[i].data->path);
		free(mi[i].data->from);
		free(mi[i].data->to);
		free(mi[i].data);
	}
	free(mi);
	for (i = 0; i < inc_len; ++i) {
		free(inc_slash[i]);
	}
	free(inc_slash);
	for (i = 0; i < exc_len; ++i) {
		free(exc_slash[i]);
	}
	free(exc_slash);
	free_tree(&rt);
	free(rt.children);
	free(revisions);
	free(include);
	free(exclude);
	free(current_line);
	free(to_delete);
	return 0;
}
