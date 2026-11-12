/* wsrun - tiny host CLI around eccles_wasm (also the differential-test driver)
 *
 *   wsrun module.wasm export [hexarg ...]     one call, results printed as hex slots
 *   wsrun module.wasm --stdin                 read "export hexarg ..." lines from stdin
 *
 * Output per call: "OK <hex> <hex> ..." or "TRAP <reason>". Loads through the
 * file path API (so the chunked streaming loader is what gets exercised). */
#include "eccles_wasm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void call_line(char *line) {
    char *tok[64]; int n = 0; uint64_t args[60], res[8]; int i;
    for (char *t = strtok(line, " \t\r\n"); t && n < 64; t = strtok(NULL, " \t\r\n")) tok[n++] = t;
    if (n == 0) return;
    for (i = 1; i < n; i++) args[i - 1] = strtoull(tok[i], NULL, 16);
    memset(res, 0, sizeof res);
    if (eccles_wasm_call(tok[0], args, (uint32_t)(n - 1), res, 8) == ECCLES_WASM_SUCCESSFUL) {
        printf("OK");
        /* number of results = unknown to the CLI: print all 8, trailing zeros trimmed by the caller */
        for (i = 0; i < 8; i++) printf(" %llx", (unsigned long long)res[i]);
        printf("\n");
    } else {
        printf("TRAP %s\n", eccles_wasm_error_name(eccles_wasm_last_error()));
    }
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: wsrun file.wasm export [hexarg...] | --stdin\n"); return 2; }
    if (eccles_wasm_load(argv[1]) != ECCLES_WASM_SUCCESSFUL) {
        printf("LOADFAIL %s info=%u %s\n", eccles_wasm_error_name(eccles_wasm_last_error()),
               (unsigned)eccles_wasm_last_error_info(), eccles_wasm_last_error_str());
        return 1;
    }
    if (strcmp(argv[2], "--stdin") == 0) {
        char line[1024];
        while (fgets(line, sizeof line, stdin)) call_line(line);
    } else {
        char line[1024] = ""; int i;
        for (i = 2; i < argc; i++) { strcat(line, argv[i]); strcat(line, " "); }
        call_line(line);
    }
    return 0;
}
