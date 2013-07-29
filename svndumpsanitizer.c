/*
	svndumpsanitizer version 1.2.0, released 25 Jul 2013

	Copyright 2011,2012,2013 Daniel Suni

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

// WIN32 modification by $ergi0
#ifdef WIN32
#define _CRT_SECURE_NO_WARNINGS
#include <sys/types.h>
#define fseeko _fseeki64
#endif

#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
typedef struct {
	char *path;
	char *copyfrom;
	int action;
	int wanted;
} node;

typedef struct {
	node *nodes;
	int size;
	int number; // Used only if the revision number is changed due to empty revisions being dropped.
} revision;

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
	printf("\t\tRedefines the repository root. This option can only be used with the include option.\n");
	printf("\t\tThe path provided to this option must be the beginning of (or the whole) path\n");
	printf("\t\tprovided to the include option. If more than one path is provided you can provide\n");
	printf("\t\ta path only up to the point where the paths diverge.\n\n");
	printf("\t\tE.g.\n\t\t\"-n foo/bar/trunk -r foo/bar\" - OK. (This is probably the typical case.)\n");
	printf("\t\t\"-n foo/bar/trunk -r foo/bar/trunk\" - OK.\n");
	printf("\t\t\"-n foo/bar/trunk foo/baz/trunk -r foo\" - OK.\n");
	printf("\t\t\"-n foo/bar/trunk foo/baz/trunk -r foo/bar\" - WRONG.\n");
	exit(0);
}

char* str_malloc(size_t sz) {
	char* str;
	if ((str = (char*)malloc(sz)) == NULL) {
		exit_with_error("malloc failed", 2);
	}
	return str;
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

//Found at http://icodesnippet.com/snippet/cpp/wildcard-search-dreamincode
// WILDCARD SEARCH FUNCTIONS
 
// Returns:1 on match, 0 on no match.
//
int wildcmp(char *wild, char *string) {
    char *cp, *mp;
        while ((*string) && (*wild != '*')) {
            if ((*wild != *string) && (*wild != '?')) {
            return 0;
        }
 
        wild++;
        string++;
    }
 
while (*string) {
            if (*wild == '*') {
                if (!*++wild) {
                return 1;
            }
 
            mp = wild;
            cp = string+1;
            } else if ((*wild == *string) || (*wild == '?')) {
 
            wild++;
            string++;
            } else {
 
            wild = mp;
            string = cp++;
        }
 
    }
 
while (*wild == '*') {
        wild++;
    }
 
return !*wild;
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
	strcpy(str, &path[i + 1]);
	return str;
}

int main(int argc, char **argv) {
	// Misc temporary variables
	int i, j, k, l, ch, want_by_default, should_do, new_number, empty, temp_int;
	off_t offset,con_len;
	time_t rawtime;
	struct tm *ptm;
	char *temp_str = NULL;
	char *temp_str2 = NULL;
	char *tok_str = NULL;
	int steps = 6;
	int cur_step = 1;
	int delete_needed = 0;
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
	char **mustkeep = NULL; // For storing the paths the user wants to discard, but must be kept
	char **relevant_paths = NULL;
	char **no_longer_relevant = NULL;
	char *redefined_root = NULL;

	// Variables to hold the size of 2D pseudoarrays
	int inc_len = 0;
	int exc_len = 0;
	int must_len = 0;
	int rel_len = 0;
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

	// Analyze the given parameters
	for (i = 1 ; i < argc ; ++i) {
		if (starts_with(argv[i], "-")) {
			if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
				free(current_line);
				free(revisions);
				free(include);
				free(exclude);
				show_help_and_exit();
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
				steps = 7;
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
		temp_str = str_malloc(strlen(redefined_root) + 2);
		strcpy(temp_str, redefined_root);
		strcat(temp_str, "/");
		for (i = 0; i < inc_len; ++i) {
			if (!(strcmp(include[i], redefined_root) == 0 || starts_with(include[i], temp_str))) {
				fclose(infile);
				if (outfile != NULL) {
					fclose(outfile);
				}
				strcat(redefined_root, " can not be redefined as root for include ");
				exit_with_error(strcat(redefined_root, include[i]), 1);
			}
		}
		free(temp_str);
	}
	want_by_default = (include == NULL);
	fprintf(messages, "Step %d/%d: Reading the infile... ", cur_step, steps);
	fflush(messages);
	++cur_step;

	// Read the metadata from all nodes.
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
				else if (starts_with(current_line,"Content-length: ")) {
					// The content is irrelevant (and might mess things up). Skip it.
					fseeko(infile, (off_t)atol(&current_line[16]) + CONTENT_PADDING, SEEK_CUR);
					reading_node = 0;
				}
				else if (starts_with(current_line,"Node-action: ")) {
					if (strcmp(&current_line[13],"add") == 0) {
						current_node[nod_len].action = ADD;
					}
					else if (strcmp(&current_line[13],"delete") == 0) {
						current_node[nod_len].action = DELETE;
					}
					else if (strcmp(&current_line[13],"change") == 0) {
						current_node[nod_len].action = CHANGE;
					}
					else {
						current_node[nod_len].action = REPLACE;
					}
				}
				else if (starts_with(current_line,"Node-copyfrom-path: ")) {
					current_node[nod_len].copyfrom = str_malloc(strlen(&current_line[19]));
					strcpy(current_node[nod_len].copyfrom, &current_line[20]);
				}
			} // End of "if (reading_node)"
			else if (starts_with(current_line,"Node-path: ")) {
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
				current_node[nod_len].path = str_malloc(strlen(&current_line[10]));
				strcpy(current_node[nod_len].path, &current_line[11]);
				current_node[nod_len].copyfrom = NULL;
				current_node[nod_len].wanted = want_by_default;
				reading_node = 1;
			}
			else if (starts_with(current_line,"Revision-number: ")) {
				if (rev_len >= 0) {
					revisions[rev_len].nodes = current_node;
				}
				++rev_len;
				if (rev_len == rev_max) {
					rev_max += INCREMENT;
					if ((revisions = (revision*)realloc(revisions, (rev_max * sizeof(revision)))) == NULL) {
						exit_with_error("realloc failed", 2);
					}
				}
				current_node = NULL;
				revisions[rev_len].nodes = NULL;
				revisions[rev_len].size = 0;
				revisions[rev_len].number = -1;
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
	fprintf(messages, "OK\nStep %d/%d: Removing unwanted nodes... ", cur_step, steps);
	fflush(messages);
	++cur_step;

	// Analyze the metadata in order to decide which nodes to keep.
	// If the user specified excludes, mark nodes in the exclude paths as unwanted.
	// (By default all nodes are wanted when using excludes.)
	if (exclude != NULL) {
		for (i = rev_len - 1; i >= 0; --i) {
			for (j = 0; j < revisions[i].size; ++j) {
				for (k = 0; k < exc_len; ++k) {
					temp_str = str_malloc(strlen(exclude[k]) + 2);
					strcpy(temp_str, exclude[k]);
					strcat(temp_str, "/");
					if (strcmp(revisions[i].nodes[j].path, exclude[k]) == 0 || starts_with(revisions[i].nodes[j].path, temp_str)) {
						revisions[i].nodes[j].wanted = 0;
					}
					free(temp_str);
				}
				// Check whether the node has been marked as a "must keep". Keep it if it has.
				for (k = 0; k < must_len; ++k) {
					if ((temp_str = (char*)calloc(strlen(mustkeep[k]) + 2, 1)) == NULL) {
						exit_with_error("calloc failed", 2);
					}
					temp_str2 = str_malloc(strlen(mustkeep[k]) + 1);
					strcpy(temp_str2, mustkeep[k]);
					tok_str = strtok(temp_str2, "/");
					while (tok_str != NULL) {
						strcat(temp_str,tok_str);
						if (strcmp(revisions[i].nodes[j].path, temp_str) == 0) {
							revisions[i].nodes[j].wanted = 1;
						}
						strcat(temp_str,"/");
						tok_str = strtok(NULL, "/");
					}
					if (starts_with(revisions[i].nodes[j].path, temp_str)) {
						revisions[i].nodes[j].wanted = 1;
					}
					free(temp_str);
					free(temp_str2);
				}
				// Check whether the path should be added as a "must keep".
				if (revisions[i].nodes[j].wanted && revisions[i].nodes[j].copyfrom != NULL) {
					should_do = 0;
					for (k = 0; k < exc_len; ++k) {
						temp_str = str_malloc(strlen(exclude[k]) + 2);
						strcpy(temp_str, exclude[k]);
						strcat(temp_str, "/");
						if (strcmp(revisions[i].nodes[j].copyfrom, exclude[k]) == 0 || starts_with(revisions[i].nodes[j].copyfrom, temp_str)) {
							should_do = 1;
						}
						free(temp_str);
					}
					if (should_do) {
						if ((mustkeep = (char**)realloc(mustkeep, (must_len + 1) * sizeof(char*))) == NULL) {
							exit_with_error("realloc failed", 2);
						}
						mustkeep[must_len] = str_malloc(strlen(revisions[i].nodes[j].copyfrom) + 1);
						strcpy(mustkeep[must_len], revisions[i].nodes[j].copyfrom);
						++must_len;
					}
				}
			}
		}
	}
	// If the user specified includes, mark nodes in the include paths as wanted.
	// (By default all nodes are unwanted when using includes.)
	else {
		for (i = rev_len - 1; i >= 0; --i) {
			for (j = 0 ; j < revisions[i].size ; ++j) {                           
				for (k = 0; k < inc_len; ++k) {
					temp_str = str_malloc(strlen(include[k]) + 2);
					strcpy(temp_str, include[k]);
					strcat(temp_str, "/");
					temp_str2 = str_malloc(strlen(revisions[i].nodes[j].path) + 2);
					strcpy(temp_str2, revisions[i].nodes[j].path);
					strcat(temp_str2, "/");
					if (strcmp(revisions[i].nodes[j].path, include[k]) == 0 || starts_with(revisions[i].nodes[j].path, temp_str) || starts_with(include[k], temp_str2) || wildcmp(include[k], temp_str2)) {
						revisions[i].nodes[j].wanted = 1;
					}
					free(temp_str);
					free(temp_str2);
				}
				// Check whether the node has been marked as a "must keep".
				for (k = 0; k < must_len; ++k) {
					if ((temp_str = (char*)calloc(strlen(mustkeep[k]) + 2, 1)) == NULL) {
						exit_with_error("calloc failed", 2);
					}
					temp_str2 = str_malloc(strlen(mustkeep[k]) + 1);
					strcpy(temp_str2, mustkeep[k]);
					tok_str = strtok(temp_str2, "/");
					while (tok_str != NULL) {
						strcat(temp_str,tok_str);
						if (strcmp(revisions[i].nodes[j].path, temp_str) == 0) {
							revisions[i].nodes[j].wanted = 1;
						}
						strcat(temp_str,"/");
						tok_str = strtok(NULL, "/");
					}
					if (starts_with(revisions[i].nodes[j].path, temp_str) || wildcmp(temp_str,revisions[i].nodes[j].path)) {
						revisions[i].nodes[j].wanted = 1;
					}
					free(temp_str);
					free(temp_str2);
				}
				// Check whether the path should be added as a "must keep".
				if (revisions[i].nodes[j].wanted && revisions[i].nodes[j].copyfrom != NULL) {
					should_do = 1;
					for (k = 0; k < inc_len; ++k) {
						temp_str = str_malloc(strlen(include[k]) + 2);
						strcpy(temp_str, include[k]);
						strcat(temp_str, "/");
						if (strcmp(revisions[i].nodes[j].copyfrom, include[k]) == 0 || starts_with(revisions[i].nodes[j].copyfrom, temp_str) || wildcmp(temp_str,revisions[i].nodes[j].copyfrom)) {
							should_do = 0;
						}
						free(temp_str);						
					}
					if (should_do) {
						if ((mustkeep = (char**)realloc(mustkeep, (must_len + 1) * sizeof(char*))) == NULL) {
							exit_with_error("realloc failed", 2);
						}
						mustkeep[must_len] = str_malloc(strlen(revisions[i].nodes[j].copyfrom) + 1);
						strcpy(mustkeep[must_len], revisions[i].nodes[j].copyfrom);
						++must_len;
					}
				}
			}
		}
	}
	fprintf(messages, "OK\nStep %d/%d: Bringing back necessary delete operations... ", cur_step, steps);
	fflush(messages);
	++cur_step;

	// Parse through the metadata again - this time bringing back any
	// possible delete instructions for the nodes we were forced to keep
	// but actually don't want any more.
	for (i = 0; i < rev_len; ++i) {
		for (j = 0; j < revisions[i].size; ++j) {
			if (revisions[i].nodes[j].wanted && revisions[i].nodes[j].action != DELETE) {
				should_do = 1;
				for (k = 0; k < rel_len; ++k) {
					if (relevant_paths[k] != NULL && strcmp(revisions[i].nodes[j].path, relevant_paths[k]) == 0) {
						should_do = 0;
					}
				}
				if (should_do) {
					if ((relevant_paths = (char**)realloc(relevant_paths, (rel_len + 1) * sizeof(char*))) == NULL) {
						exit_with_error("realloc failed", 2);
					}
					relevant_paths[rel_len] = str_malloc(strlen(revisions[i].nodes[j].path) + 1);
					strcpy(relevant_paths[rel_len], revisions[i].nodes[j].path);
					++rel_len;					
				}
			}
			if (revisions[i].nodes[j].action == DELETE) {
				for (k = 0; k < rel_len; ++k) {
					if (relevant_paths[k] != NULL && strcmp(revisions[i].nodes[j].path, relevant_paths[k]) == 0) {
						revisions[i].nodes[j].wanted = 1;
						for (l = 0; l < rel_len; ++l) {
							temp_str = str_malloc(strlen(revisions[i].nodes[j].path) + 2);
							strcpy(temp_str, revisions[i].nodes[j].path);
							strcat(temp_str, "/");
							if (relevant_paths[l] != NULL && (strcmp(relevant_paths[l], revisions[i].nodes[j].path) == 0 || starts_with(relevant_paths[l], temp_str))) {
								free(relevant_paths[l]);
								relevant_paths[l] = NULL;
							}
							free(temp_str);
						}
					}
				}
			}
		}
	}
	fprintf(messages, "OK\nStep %d/%d: Identifying lingering unwanted nodes... ", cur_step, steps);
	fflush(messages);
	++cur_step;

	// Find paths which are not relevant as specified by the user, but still lingers
	// due to dependency includes. (So that we can deal with them later.)
	for (i = 0; i < rel_len; ++i) {
		if (include == NULL && relevant_paths[i] != NULL) {
			for (j = 0; j < exc_len; ++j) {
				temp_str = str_malloc(strlen(exclude[j]) + 2);
				strcpy(temp_str, exclude[j]);
				strcat(temp_str, "/");
				if (strcmp(relevant_paths[i], exclude[j]) == 0 || starts_with(relevant_paths[i], temp_str)) {
					if ((no_longer_relevant = (char**)realloc(no_longer_relevant, (no_len + 1) * sizeof(char*))) == NULL) {
						exit_with_error("realloc failed", 2);
					}
					no_longer_relevant[no_len] = str_malloc(strlen(relevant_paths[i]) + 1);
					strcpy(no_longer_relevant[no_len], relevant_paths[i]);
					++no_len;
				}
				free(temp_str);
			}
		}
		else if (exclude == NULL && relevant_paths[i] != NULL) {
			temp_str = str_malloc(strlen(relevant_paths[i]) + 2);
			strcpy(temp_str, relevant_paths[i]);
			strcat(temp_str, "/");
			for (j = 0; j < inc_len; ++j) {
				temp_str2 = str_malloc(strlen(include[j]) + 2);
				strcpy(temp_str2, include[j]);
				strcat(temp_str2, "/");
				if (!(strcmp(relevant_paths[i], include[j]) == 0 || starts_with(relevant_paths[i], temp_str2) || starts_with(include[j], temp_str))) {
					if ((no_longer_relevant = (char**)realloc(no_longer_relevant, (no_len + 1) * sizeof(char*))) == NULL) {
					exit_with_error("realloc failed", 2);
					}
					no_longer_relevant[no_len] = str_malloc(strlen(relevant_paths[i]) + 1);
					strcpy(no_longer_relevant[no_len], relevant_paths[i]);
					++no_len;
				}
				free(temp_str2);
			}
			free(temp_str);
		}
	}
	// Check that we don't have anything specifically included in our "no_longer_relevant"-section.
	for (i = 0; i < no_len; ++i) {
		if (no_longer_relevant[i] != NULL) {
			for (j = 0; j < inc_len ; ++j) {
				temp_str = str_malloc(strlen(include[j]) + 2);
				strcpy(temp_str, include[j]);
				strcat(temp_str, "/");
				if (strcmp(no_longer_relevant[i], include[j]) == 0 || starts_with(no_longer_relevant[i], temp_str)) {
					free(no_longer_relevant[i]);
					no_longer_relevant[i] = NULL;
					break;
				}
				free(temp_str);
			}
		}
	}

	// Remove any directory entries that should no longer exist with the redefined root
	if (redefined_root != NULL) {
		for (i = 0; i < rev_len ; ++i) {
			for (j = 0; j < revisions[i].size; ++j) {
				if (revisions[i].nodes[j].wanted) {
					temp_str = str_malloc(strlen(redefined_root) + 2);
					strcpy(temp_str, redefined_root);
					strcat(temp_str, "/");
					for (k = strlen(temp_str) - 1; k > 0; --k) {
						if (temp_str[k] == '/') {
							temp_str[k] = '\0';
							if (strcmp(temp_str, revisions[i].nodes[j].path) == 0) {
								revisions[i].nodes[j].wanted = 0;
								for (l = 0; l < no_len; ++l) {
									if (strcmp(revisions[i].nodes[j].path, no_longer_relevant[l]) == 0) {
										free(no_longer_relevant[l]);
										no_longer_relevant[l] = NULL;
									}
								}
							}
						}
					}
					free(temp_str);
				}
			}
		}
		// Reduce the paths of deletion candidates, so that we delete the correct paths
		for (i = 0; i < no_len; ++i) {
			if (no_longer_relevant[i] != NULL) {
				temp_str = reduce_path(redefined_root, no_longer_relevant[i]);
				strcpy(no_longer_relevant[i], temp_str);
				free(temp_str);
			}
		}
	}

	// Remove redundant entries (i.e. delete only "trunk" instead of "trunk", "trunk/foo", "trunk/bar", et.c.)
	for (i = 0; i < no_len; ++i) {
		if (no_longer_relevant[i] != NULL) {
			delete_needed = 1;
			temp_str = str_malloc(strlen(no_longer_relevant[i]) + 2);
			strcpy(temp_str, no_longer_relevant[i]);
			strcat(temp_str, "/");
			for (j = 0; j < no_len; ++j) {
				if (i != j && no_longer_relevant[j] != NULL && (starts_with(no_longer_relevant[j], temp_str) || strcmp(no_longer_relevant[i], no_longer_relevant[j]) == 0)) {
					free(no_longer_relevant[j]);
					no_longer_relevant[j] = NULL;
				}
			}			
			for (j = 0; j < inc_len ; ++j) {
				if (strcmp(no_longer_relevant[i], include[j]) == 0 || starts_with(include[j], temp_str)) {
					free(no_longer_relevant[i]);
					no_longer_relevant[i] = NULL;
					break;
				}
			}
			free(temp_str);
		}
	}

	// Renumber the revisions if the empty ones are to be dropped
	if (drop_empty) {
		fprintf(messages, "OK\nStep %d/%d: Renumbering revisions... ", cur_step, steps);
		fflush(messages);
		++cur_step;
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
			if (!empty) {
				revisions[i].number = new_number;
				++new_number;
			}
		}
	}
	fprintf(messages, "OK\nStep %d/%d: Writing the outfile... ", cur_step, steps);
	fflush(messages);
	++cur_step;

	// Copy the infile to the outfile skipping the undesireable parts.
	reading_node = 0;
	fseeko(infile, 0 , SEEK_SET);
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
				else if(writing && redefined_root != NULL && starts_with(current_line, "Node-copyfrom-path: ")) {
					temp_str = reduce_path(redefined_root, &current_line[20]);
					fprintf(outfile, "Node-copyfrom-path: %s\n", temp_str);
					toggle = 1;
					free(temp_str);	
				}
				else if (starts_with(current_line, "Content-length: ")) {
					con_len = (off_t)atol(&current_line[16]);
					if (writing) {
						fprintf(outfile, "%s\n", current_line);
						for (offset = 0; offset < con_len + CONTENT_PADDING; ++offset) {
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
				nod = -1;
				writing = (!drop_empty || revisions[rev].number >= 0);
				if (drop_empty && writing) {
					temp_int = atoi(&current_line[17]);
					fprintf(outfile, "Revision-number: %d\n", revisions[temp_int].number);
					toggle = 1;
				}
			}
			if (writing && !toggle) {
				fprintf(outfile, "%s\n", current_line);
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
	fprintf(messages, "OK\nStep %d/%d: Adding revision deleting surplus nodes... ", cur_step, steps);
	fflush(messages);
	++cur_step;

	// Now we deal with any surplus nodes by adding a revision that deletes them.
	if (delete_needed) {
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
		for (i = 0; i < no_len; ++i) {
			if (no_longer_relevant[i] != NULL) {
				fprintf(outfile, "Node-path: %s\n", no_longer_relevant[i]);
				fprintf(outfile, "Node-action: delete\n\n\n");
			}
		}
		fprintf(messages, "OK\n");
	}
	else {
		fprintf(messages, "NOT NEEDED\n");
	}

	// Clean everything up
	fclose(infile);
	if (to_file) {
		fclose(outfile);
	}
	for (i = 0; i < rev_len; ++i) {
		for (j = 0; j < revisions[i].size; ++j) {
			free(revisions[i].nodes[j].path);
			free(revisions[i].nodes[j].copyfrom);
		}
		free(revisions[i].nodes);
	}
	for (i = 0; i < rel_len; ++i) {
		free(relevant_paths[i]);
	}
	for (i = 0; i < no_len; ++i) {
		free(no_longer_relevant[i]);
	}
	for (i = 0; i < must_len; ++i) {
		free(mustkeep[i]);
	}
	free(revisions);
	free(relevant_paths);
	free(no_longer_relevant);
	free(include);
	free(exclude);
	free(mustkeep);
	free(current_line);

	return 0;
}
