/*
	svndumpsanitizer version 2.0.0 Beta 1, released 15 Nov 2015

	Copyright 2011,2012,2013,2014,2015 Daniel Suni

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

#define SDS_VERSION "2.0.0 Beta 1"
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
	printf("\t-r, --redefine-root [PATH]\n");
	printf("\t\tDOES NOT WORK WITH THIS VERSION. Will display a warning and do nothing if used.\n\n");
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

/*******************************************************************************
 *
 * Include/exclude-related functions
 *
 ******************************************************************************/

int is_excluded(char *path, char **excludes, char **ex_slash, int ex_len) {
	int i;
	for (i = 0; i < ex_len; ++i) {
		if (strcmp(path, excludes[i]) == 0 || starts_with(path, ex_slash[i])) {
			return 1;
		}
	}
	return 0;
}

// Dumb exclude that just marks excludes as unwanted. We'll have to come back
// later to check which ones are still needed due to dependencies
void parse_exclude_preparation(revision *r, char **excludes, char **ex_slash, int rev_len, int ex_len) {
	int i, j;
	for (i = 0; i < rev_len; ++i) {
		for (j = 0; j < r[i].size; ++j) {
			if (is_excluded(r[i].nodes[j].path, excludes, ex_slash, ex_len)) {
				r[i].nodes[j].wanted = 0;
			}
		}
		for (j = 0; j < r[i].fake_size; ++j) {
			if (is_excluded(r[i].fakes[j]->path, excludes, ex_slash, ex_len)) {
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
	if (fail_if_not_found) {
		fprintf(stderr, "Could not find requested subtree: %s\n", path);
		exit_with_error("Internal logic error.", 3);
	}
	return NULL;
}

void restore_delete_node_if_needed(repotree *rt, node *n) {
	repotree *target = get_subtree(rt, n->path, 1);
	int i = target->map_len - 1;
	// Find the right pointer...
	while (i >= 0 && target->map[i] != n) {
		--i;
	}
	--i; // And back up to the previous one...
	while (i >= 0 && target->map[i]->action != DELETE) {
		if (target->map[i]->wanted) {
			n->wanted = 2;
			return;
		}
		--i;
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

// Add event to repo tree. If the path in question does not yet exist, we add it to the tree.
// If it does exists we update all map nodes from the current revision forward to the current
// node, or in case of DELETE node, set them to NULL. Dependencies are added for non-ADD-type
// nodes in case another event affects the same path during the same revision. ADD-types are
// either new files (=doesn't need this dependency) or copyfrom instances (=needs different
// dependecy, handled elsewhere).
void add_event(repotree *rt, node *n) { //, char *str, int rev_len) {
	int i;
	for (i = 0; i < rt->chi_len; ++i) {
		if (matches_path_start(n->path, rt->children[i].path)) {
			if (strcmp(n->path, rt->children[i].path) != 0) {
				//add_event(&rt->children[i], n, &str[j + 1], rev_len);
				add_event(&rt->children[i], n); //, str, rev_len);
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
node** get_relevant_nodes_at_revision(repotree *rt, int rev, int *size) {
	int i, j, ch_size;
	int self_size = 0;
	node **nptr, **nptr2;
	node *n;
	for (i = 0; i < rt->chi_len; ++i) {
		n = get_node_at_revision(rt->children[i].map, rev, rt->children[i].map_len);
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
		n = get_node_at_revision(rt->children[i].map, rev, rt->children[i].map_len);
		if (n && n->action != DELETE) {
			nptr[self_size] = n;
			++self_size;
		}
	}
	for (i = 0; i < rt->chi_len; ++i) {
		n = get_node_at_revision(rt->children[i].map, rev, rt->children[i].map_len);
		if (n && n->action != DELETE) {
			nptr2 = get_relevant_nodes_at_revision(&rt->children[i], rev, &ch_size);
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
		while (minfo[j] >= 48 && minfo[j] <= 57) {
			--j;
		}
		md->to[md->size] = atoi(&minfo[j + 1]);
		while (minfo[j] != ':') {
			if (minfo[j] == ',' || minfo[j] == '-') {
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
int get_mergerow_size(mergedata *data, revision *revisions, int row) {
	int to, from;
	int size = 0;
	to = revisions[data->to[row]].number;
	from = revisions[data->from[row]].number;
	if (to == from) {
		size += num_len(to) + 1; // ":XXX"
	}
	else {
		size += num_len(to) + num_len(from) + 2; // ":XXX-YYY"
	}
	size += strlen(data->path[row]) + 2; // "/...\n"
	return size;
}

void write_mergeinfo(FILE *outfile, mergedata *data, revision *revisions, int orig_size, off_t con_len, off_t pcon_len) {
	int i, v_size, diff, to, from;
	int size = 0;
	for (i = 0; i < data->size; ++i) {
		size += get_mergerow_size(data, revisions, i);
	}
	v_size = size - 1; // The last (first?) newline doesn't count towards value length.
	size += num_len(v_size) + 3; // "V XXX\n"
	size += 30; // "\nK 13\nsvn:mergeinfo\n"  ... "\nPROPS-END"
	diff = orig_size - size;
	fprintf(outfile, "Prop-content-length: %d\nContent-length: %d\n\nK 13\nsvn:mergeinfo\nV %d\n",
					(int)pcon_len - diff, (int)con_len - diff, v_size);
	for (i = 0; i < data->size; ++i) {
		to = revisions[data->to[i]].number;
		from = revisions[data->from[i]].number;
		if (to == from) {
			fprintf(outfile, "/%s:%d\n", data->path[i], to);
		}
		else {
			fprintf(outfile, "/%s:%d-%d\n", data->path[i], from, to);
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
 	int i, j, k, ch, want_by_default, new_number, empty, temp_int, is_dir;
	off_t offset, con_len, pcon_len;
	char *temp_str = NULL;
	char *temp_str2 = NULL;
	char *minfo = NULL;
	int to_file = 1;

	// Variables to help analyze user input 
	int in = 0;
	int out = 0;
	int incl = 0;
	int excl = 0;
	int drop = 0;
	int redef = 0;

	// Variables related to files and paths
	FILE *infile = NULL;
	FILE *outfile = NULL;
	FILE *messages = stdout;
	char **include = NULL; // Holds the paths the user wants to keep
	char **exclude = NULL; // Holds the paths the user wants to discard
	char **exc_slash = NULL;
	char **inc_slash = NULL;
	char *redefined_root = NULL;

	// Variables to hold the size of 2D pseudoarrays
	int inc_len = 0;
	int exc_len = 0;
	int no_len = 0;
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
			if (!(in || out || incl || excl || drop || redef)) {
				exit_with_error(strcat(argv[i], " is not a valid parameter. Use -h for help."), 1);
			}
			else if (drop) {
				drop_empty = 1;
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
			fprintf(stderr, "WARNING: Redefined root does not work in this version, and will be ignored.\n");
			//redefined_root = argv[i];
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
					node_ptr = get_relevant_nodes_at_revision(subtree, revisions[i].nodes[j].copyfrom_rev, &temp_int);
				}
				else {
					subtree = get_subtree(&rt, revisions[i].nodes[j].path, 1);
					node_ptr = get_relevant_nodes_at_revision(subtree, i, &temp_int);
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
				for (k = 0; k < inc_len; ++k) {
					if (starts_with(revisions[i].nodes[j].path, inc_slash[k]) || strcmp(revisions[i].nodes[j].path, include[k]) == 0) {
						set_wanted(&revisions[i].nodes[j]);
					}
				}
			}
			for (j = 0; j < revisions[i].fake_size; ++j) {
				for (k = 0; k < inc_len; ++k) {
					if (starts_with(revisions[i].fakes[j]->path, inc_slash[k]) || strcmp(revisions[i].fakes[j]->path, include[k]) == 0) {
						set_wanted(revisions[i].fakes[j]);
					}
				}
			}
		}
		for (i = 0; i < inc_len; ++i) {
			free(inc_slash[i]);
		}
		free(inc_slash);
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
				if (!is_excluded(revisions[i].nodes[j].path, exclude, exc_slash, exc_len)) {
					set_wanted(&revisions[i].nodes[j]);
				}
			}
		}
		for (i = rev_len - 1; i > 0; --i) {
			for (j = 0; j < revisions[i].fake_size; ++j) {
				if (!is_excluded(revisions[i].fakes[j]->path, exclude, exc_slash, exc_len)) {
					set_wanted(revisions[i].fakes[j]);
				}
			}
		}
		for (i = 0; i < exc_len; ++i) {
			free(exc_slash[i]);
		}
		free(exc_slash);
	}

	// Restore wanted delete nodes
	for (i = 0; i < rev_len; ++i) {
		for (j = 0; j < revisions[i].size; ++j) {
			if (revisions[i].nodes[j].action == DELETE && !revisions[i].nodes[j].wanted) {
				restore_delete_node_if_needed(&rt, &revisions[i].nodes[j]);
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

	/***********************************************************************************
	 *
	 * Write the outfile
	 *
	 ***********************************************************************************/
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
							write_mergeinfo(outfile, mi[act_mi].data, revisions, mi[act_mi].orig_size, con_len, pcon_len);
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
	fprintf(messages, "\nAll done.\n");

	// Clean everything up
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
	free_tree(&rt);
	free(rt.children);
	free(revisions);
	free(include);
	free(exclude);
	free(current_line);	
	return 0;
}
