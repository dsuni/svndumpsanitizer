/*
dumpstrip-0.1 Copyright 2011 Daniel Suni

May be distrubuted under the terms of the GNU GPL v3 or later.

Compile with "gcc dumpstrip.c -o dumpstrip"

Use with "dumpstrip --infile foo.dump --outfile bar.dump"
 */
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONTENT_PADDING 2
#define INCREMENT 10
#define NEWLINE 10

void exit_with_error(char *message, int exit_code) {
	fprintf(stderr,"ERROR: %s\n", message);
	exit(exit_code);
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
	int i, ch;
	int in = 0;
	int out = 0;
	int cur_len = 0;
	int cur_max = 80;
	int reading_node = 0;
	off_t con_len;
	FILE *infile = NULL;
	FILE *outfile = NULL;
	char *current_line;
	if ((current_line = (char*)calloc(cur_max, 1)) == NULL) {
		exit_with_error("calloc failed", 2);
	}

	// Analyze the given parameters
	for (i = 1 ; i < argc ; ++i) {
		if (starts_with(argv[i], "-")) {
			in = (!strcmp(argv[i], "--infile") || !strcmp(argv[i], "-i"));
			out = (!strcmp(argv[i], "--outfile") || !strcmp(argv[i], "-o"));
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
		else {
			exit_with_error(strcat(argv[i], " is not a valid parameter"), 1);
		}
	}
	if (infile == NULL) {
		exit_with_error("You must specify an infile", 1);
	}
	if (outfile == NULL) {
		exit_with_error("You must specify an outfile", 1);
	}

	// Copy the infile to the outfile skipping the data.
	reading_node = 0;
	while ((ch = fgetc(infile)) != EOF) {
		if (ch == NEWLINE) {
			if (reading_node) {
				if (strlen(current_line) == 0) {
					reading_node = 0;
				}
				else if (starts_with(current_line, "Content-length: ")) {
					con_len = (off_t)atol(&current_line[16]);
					fseeko(infile, con_len + CONTENT_PADDING, SEEK_CUR);
					reading_node = 0;
				}
			}
			else if (starts_with(current_line, "Node-path: ")) {
				reading_node = 1;
			}
			fprintf(outfile, "%s\n", current_line);
			current_line[0] = '\0';
			cur_len = 0;
		}
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
	}

	// Clean everything up
	fclose(infile);
	fclose(outfile);
	free(current_line);

	return 0;
}
