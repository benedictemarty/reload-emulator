#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#define BYTES_PER_LINE 16

static void print_usage(const char* argv0) {
    fprintf(stderr,
            "Usage: %s -i binary_file -o header_file -a array_name\n"
            "\t-h show this help\n",
            argv0);
    exit(1);
}

// Valid C identifier (letters, digits, '_', not starting with a digit)
static int valid_identifier(const char* name) {
    if (!name || !*name || (*name >= '0' && *name <= '9')) return 0;
    for (const char* p = name; *p; p++) {
        const int ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_';
        if (!ok) return 0;
    }
    return 1;
}

// Returns 0 on success; a read or write error is reported instead of looping forever
static int print_file(FILE* in, FILE* out, const char* array_name) {
    int bytes = 0;
    int c;

    fprintf(out, "#pragma once\n\n");
    fprintf(out, "// clang-format off\n");
    fprintf(out, "uint8_t __in_flash() %s[] = {", array_name);

    while ((c = fgetc(in)) != EOF) {
        if (bytes) {
            fprintf(out, ", ");
        }
        if ((bytes % BYTES_PER_LINE) == 0) {
            fprintf(out, "\n\t");
        }
        bytes++;
        fprintf(out, "0x%02X", c);
    }
    if (ferror(in)) {
        fprintf(stderr, "Read error\n");
        return 1;
    }
    if (bytes) {
        fprintf(out, "\n");
    }
    fprintf(out, "};\n");
    fprintf(out, "// clang-format on\n");
    if (ferror(out)) {
        fprintf(stderr, "Write error\n");
        return 1;
    }
    return 0;
}

static int convert_bin_to_hdr(const char* infile, const char* outfile, const char* array_name) {
    FILE* in = fopen(infile, "rb");
    if (!in) {
        fprintf(stderr, "Failed to open file for reading: %s\n", infile);
        return 1;
    }
    FILE* out = fopen(outfile, "w");
    if (!out) {
        fprintf(stderr, "Failed to open file for writing: %s\n", outfile);
        fclose(in);
        return 1;
    }
    int rc = print_file(in, out, array_name);
    fclose(in);
    if (fclose(out) != 0) rc = 1;
    if (rc) remove(outfile);   // No partial header left behind
    return rc;
}

int main(int argc, char* const argv[]) {
    const char *infile = NULL, *outfile = NULL, *array_name = NULL;
    int opt;

    while ((opt = getopt(argc, argv, "i:o:a:h")) != -1) {
        switch (opt) {
            case 'i':
                infile = optarg;
                break;
            case 'o':
                outfile = optarg;
                break;
            case 'a':
                array_name = optarg;
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return 0;
        }
    }

    if (!infile || !outfile || !array_name) {
        print_usage(argv[0]);
    }
    if (!valid_identifier(array_name)) {
        fprintf(stderr, "Not a valid C identifier: %s\n", array_name);
        return 1;
    }
    return convert_bin_to_hdr(infile, outfile, array_name);
}
