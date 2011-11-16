/*
	svndumpsanitizer version 0.7.0, released 14 Nov 2011

	Copyright 2011 Daniel Suni

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
} revision;

void exit_with_error(char *message, int exit_code) {
	fprintf(stderr, "ERROR: %s\n", message);
	exit(exit_code);
}

void show_help_and_exit() {
	printf("svndumpsanitizer usage:\n\n");
	printf("svndumpsanitizer [-i, --infile INFILE] [-o, --outfile OUTFILE] [OPTIONS]\n\n");
	printf("OPTIONS\n");
	printf("\t-n, --include [PATHS]\n");
	printf("\t\tList of repository paths to include.\n\n");
	printf("\t-e, --exclude [PATHS]\n");
	printf("\t\tList of repository paths to include.\n\n");
	printf("\t\tYou must specify at least one path to include OR at least one path to exclude\n");
	printf("\t\tYou may not specify includes and excludes at the same time.\n\n");
	printf("\t\tPATHS are space separated paths as they appear in the svn repository\n");
	printf("\t\twithout leading or trailing slashes. E.g. \"-n trunk/foo/bar branches/baz\"\n");
	exit(0);
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

int main(int argc, char **argv) {
	// Misc temporary variables
	int i, j, k, l, ch, want_by_default, should_do;
	off_t offset,con_len;
	time_t rawtime;
	struct tm *ptm;
	char *temp_str = NULL;
	char *temp_str2 = NULL;
	char *tok_str = NULL;

	// Variables to help analyze user input 
	int in = 0;
	int out = 0;
	int incl = 0;
	int excl = 0;

	// Variables related to files and paths
	FILE *infile = NULL;
	FILE *outfile = NULL;
	char **include = NULL; // Holds the paths the user wants to keep
	char **exclude = NULL; // Holds the paths the user wants to discard
	char **mustkeep = NULL; // For storing the paths the user wants to discard, but must be kept
	char **relevant_paths = NULL;
	char **no_longer_relevant = NULL;

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
			if (!(in || out || incl || excl)) {
				exit_with_error(strcat(argv[i], " is not a valid parameter. Use -h for help."), 1);
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
			include[inc_len] = argv[i];
			++inc_len;			
		}
		else if (excl) {
			if ((exclude = (char**)realloc(exclude, (exc_len + 1) * sizeof(char*))) == NULL) {
				exit_with_error("realloc failed", 2);
			}
			exclude[exc_len] = argv[i];
			++exc_len;
		}
		else {
			exit_with_error(strcat(argv[i], " is not a valid parameter. Use -h for help."), 1);
		}
	}
	if (infile == NULL) {
		exit_with_error("You must specify an infile", 1);
	}
	if (outfile == NULL) {
		exit_with_error("You must specify an outfile", 1);
	}
	if (include == NULL && exclude == NULL) {
		fclose(infile);
		fclose(outfile);
		exit_with_error("You must specify something to either include or exclude", 1);
	}
	if (include != NULL && exclude != NULL) {
		fclose(infile);
		fclose(outfile);
		exit_with_error("You may not specify both includes and excludes", 1);
	}
	want_by_default = (include == NULL);
	printf("Step 1/6: Reading the infile... ");

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
					if ((current_node[nod_len].copyfrom = (char*)malloc(strlen(&current_line[19]))) == NULL) {
						exit_with_error("malloc failed", 2);
					}
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
				if ((current_node[nod_len].path = (char*)malloc(strlen(&current_line[10]))) == NULL) {
					exit_with_error("malloc failed", 2);
				}
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
	printf("OK\nStep 2/6: Removing unwanted nodes... ");

	// Analyze the metadata in order to decide which nodes to keep.
	// If the user specified excludes, mark nodes in the exclude paths as unwanted.
	// (By default all nodes are wanted when using excludes.)
	if (exclude != NULL) {
		for (i = rev_len - 1; i >= 0; --i) {
			for (j = 0; j < revisions[i].size; ++j) {
				for (k = 0; k < exc_len; ++k) {
					if ((temp_str = (char*)malloc(strlen(exclude[k]) + 2)) == NULL) {
						exit_with_error("malloc failed", 2);
					}
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
					if ((temp_str2 = (char*)malloc(strlen(mustkeep[k]) + 1)) == NULL) {
						exit_with_error("malloc failed", 2);
					}
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
						if ((temp_str = (char*)malloc(strlen(exclude[k]) + 2)) == NULL) {
							exit_with_error("malloc failed", 2);
						}
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
						if ((mustkeep[must_len] = (char*)malloc(strlen(revisions[i].nodes[j].copyfrom) + 1)) == NULL) {
							exit_with_error("malloc failed", 2);
						}
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
					if ((temp_str = (char*)malloc(strlen(include[k]) + 2)) == NULL) {
						exit_with_error("malloc failed", 2);
					}
					strcpy(temp_str, include[k]);
					strcat(temp_str, "/");
					if ((temp_str2 = (char*)malloc(strlen(revisions[i].nodes[j].path) + 2)) == NULL) {
						exit_with_error("malloc failed", 2);
					}
					strcpy(temp_str2, revisions[i].nodes[j].path);
					strcat(temp_str2, "/");
					if (strcmp(revisions[i].nodes[j].path, include[k]) == 0 || starts_with(revisions[i].nodes[j].path, temp_str) || starts_with(include[k], temp_str2)) {
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
					if ((temp_str2 = (char*)malloc(strlen(mustkeep[k]) + 1)) == NULL) {
						exit_with_error("malloc failed", 2);
					}
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
					should_do = 1;
					for (k = 0; k < inc_len; ++k) {
						if ((temp_str = (char*)malloc(strlen(include[k]) + 2)) == NULL) {
							exit_with_error("malloc failed", 2);
						}
						strcpy(temp_str, include[k]);
						strcat(temp_str, "/");
						if (strcmp(revisions[i].nodes[j].copyfrom, include[k]) == 0 || starts_with(revisions[i].nodes[j].copyfrom, temp_str)) {
							should_do = 0;
						}
						free(temp_str);						
					}
					if (should_do) {
						if ((mustkeep = (char**)realloc(mustkeep, (must_len + 1) * sizeof(char*))) == NULL) {
							exit_with_error("realloc failed", 2);
						}
						if ((mustkeep[must_len] = (char*)malloc(strlen(revisions[i].nodes[j].copyfrom) + 1)) == NULL) {
							exit_with_error("malloc failed", 2);
						}
						strcpy(mustkeep[must_len], revisions[i].nodes[j].copyfrom);
						++must_len;
					}
				}
			}
		}
	}
	printf("OK\nStep 3/6: Bringing back necessary delete operations... ");

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
					if ((relevant_paths[rel_len] = (char*)malloc(strlen(revisions[i].nodes[j].path) + 1)) == NULL) {
						exit_with_error("malloc failed", 2);
					}
					strcpy(relevant_paths[rel_len], revisions[i].nodes[j].path);
					++rel_len;					
				}
			}
			if (revisions[i].nodes[j].action == DELETE) {
				for (k = 0; k < rel_len; ++k) {
					if (relevant_paths[k] != NULL && strcmp(revisions[i].nodes[j].path, relevant_paths[k]) == 0) {
						revisions[i].nodes[j].wanted = 1;
						for (l = 0; l < rel_len; ++l) {
							if ((temp_str = (char*)malloc(strlen(revisions[i].nodes[j].path) + 2)) == NULL) {
								exit_with_error("malloc failed", 2);
							}
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
	printf("OK\nStep 4/6: Identifying lingering unwanted nodes... ");

	// Find paths which are not relevant as specified by the user, but still lingers
	// due to dependency includes. (So that we can deal with them later.)
	for (i = 0; i < rel_len; ++i) {
		if (include == NULL && relevant_paths[i] != NULL) {
			for (j = 0; j < exc_len; ++j) {
				if ((temp_str = (char*)malloc(strlen(exclude[j]) + 2)) == NULL) {
					exit_with_error("malloc failed", 2);
				}
				strcpy(temp_str, exclude[j]);
				strcat(temp_str, "/");
				if (strcmp(relevant_paths[i], exclude[j]) == 0 || starts_with(relevant_paths[i], temp_str)) {
					if ((no_longer_relevant = (char**)realloc(no_longer_relevant, (no_len + 1) * sizeof(char*))) == NULL) {
						exit_with_error("realloc failed", 2);
					}
					if ((no_longer_relevant[no_len] = (char*)malloc(strlen(relevant_paths[i]) + 1)) == NULL) {
						exit_with_error("malloc failed", 2);
					}
					strcpy(no_longer_relevant[no_len], relevant_paths[i]);
					++no_len;
				}
				free(temp_str);
			}
		}
		else if (exclude == NULL && relevant_paths[i] != NULL) {
			if ((temp_str = (char*)malloc(strlen(relevant_paths[i]) + 2)) == NULL) {
				exit_with_error("malloc failed", 2);
			}
			strcpy(temp_str, relevant_paths[i]);
			strcat(temp_str, "/");
			for (j = 0; j < inc_len; ++j) {
				if ((temp_str2 = (char*)malloc(strlen(include[j]) + 2)) == NULL) {
					exit_with_error("malloc failed", 2);
				}
				strcpy(temp_str2, include[j]);
				strcat(temp_str2, "/");
				if (!(strcmp(relevant_paths[i], include[j]) == 0 || starts_with(relevant_paths[i], temp_str2) || starts_with(include[j], temp_str))) {
					if ((no_longer_relevant = (char**)realloc(no_longer_relevant, (no_len + 1) * sizeof(char*))) == NULL) {
						exit_with_error("realloc failed", 2);
					}
					if ((no_longer_relevant[no_len] = (char*)malloc(strlen(relevant_paths[i]) + 1)) == NULL) {
						exit_with_error("malloc failed", 2);
					}
					strcpy(no_longer_relevant[no_len], relevant_paths[i]);
					++no_len;
				}
				free(temp_str2);
			}
			free(temp_str);
		}
	}
	// Remove redundant entries (i.e. delete only "trunk" instead of "trunk", "trunk/foo", "trunk/bar", et.c.)
	for (i = 0; i < no_len; ++i) {
		if (no_longer_relevant[i] != NULL) {
			if ((temp_str = (char*)malloc(strlen(no_longer_relevant[i]) + 2)) == NULL) {
				exit_with_error("malloc failed", 2);
			}
			strcpy(temp_str, no_longer_relevant[i]);
			strcat(temp_str, "/");
			for (j = 0; j < no_len; ++j) {
				if (i != j && starts_with(no_longer_relevant[j], temp_str)) {
					free(no_longer_relevant[j]);
					no_longer_relevant[j] = NULL;
				}
			}
			free(temp_str);
		}
	}
	printf("OK\nStep 5/6: Writing the outfile... ");

	// Copy the infile to the outfile skipping the undesireable parts.
	reading_node = 0;
	rewind(infile);
	while ((ch = fgetc(infile)) != EOF) {
		if (ch == NEWLINE) {
			if (reading_node) {
				if (strlen(current_line) == 0) {
					reading_node = 0;
					writing = 1;
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
			}
			else if (starts_with(current_line, "Revision-number: ")) {
				++rev;
				nod = -1;
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
	printf("OK\nStep 6/6: Adding revision deleting surplus nodes... ");


	// Now we deal with any surplus nodes by adding a revision that deletes them.
	if (no_longer_relevant != NULL) {
		time(&rawtime);
		ptm = gmtime(&rawtime);
		fprintf(outfile, "Revision-number: %d\n", rev_len);
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
		printf("OK\n");
	}
	else {
		printf("NOT NEEDED\n");
	}

	// Clean everything up
	fclose(infile);
	fclose(outfile);
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
