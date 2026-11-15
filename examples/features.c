#include <iris.h>

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int check(iris_status_t status, const char *call)
{
    if (status == IRIS_OK) return 1;
    iris_error_t error = {0};
    if (status < 0) iris_last_error(&error);
    fprintf(stderr, "%s returned %" PRId32 ": %s\n", call, status,
            error.message ? error.message : "");
    return 0;
}

#define CHECK(call) check((call), #call)

static int print_text(iris_node_map_t *map, const char *name,
                      iris_status_t (*read)(iris_node_map_t *, const char *, char *, size_t *))
{
    size_t size = 0;
    if (!CHECK(read(map, name, NULL, &size))) return 0;
    char *text = malloc(size);
    if (!text) {
        fputs("Cannot allocate feature text\n", stderr);
        return 0;
    }
    const int ok = CHECK(read(map, name, text, &size));
    if (ok) printf("%s\n", text);
    free(text);
    return ok;
}

static int print_value(iris_node_map_t *map, const char *name, iris_feature_type_t type)
{
    printf("%s = ", name);
    switch (type) {
    case IRIS_FEATURE_INTEGER: {
        int64_t value;
        if (!CHECK(iris_feature_get_integer(map, name, &value))) return 0;
        printf("%" PRId64 "\n", value);
        return 1;
    }
    case IRIS_FEATURE_FLOAT: {
        double value;
        if (!CHECK(iris_feature_get_float(map, name, &value))) return 0;
        printf("%g\n", value);
        return 1;
    }
    case IRIS_FEATURE_BOOLEAN: {
        uint32_t value;
        if (!CHECK(iris_feature_get_boolean(map, name, &value))) return 0;
        puts(value ? "true" : "false");
        return 1;
    }
    case IRIS_FEATURE_STRING:
        return print_text(map, name, iris_feature_get_string);
    case IRIS_FEATURE_ENUMERATION:
        return print_text(map, name, iris_feature_get_enumeration);
    default:
        return print_text(map, name, iris_feature_to_string);
    }
}

static int print_constraints(iris_node_map_t *map, const char *name, iris_feature_type_t type)
{
    if (type == IRIS_FEATURE_INTEGER) {
        iris_integer_range_t range;
        if (!CHECK(iris_feature_integer_range(map, name, &range))) return 0;
        printf("Range %" PRId64 " to %" PRId64, range.minimum, range.maximum);
        if (range.increment_mode == IRIS_INCREMENT_FIXED)
            printf(", step %" PRId64, range.increment);
        putchar('\n');
    } else if (type == IRIS_FEATURE_FLOAT) {
        iris_float_range_t range;
        if (!CHECK(iris_feature_float_range(map, name, &range))) return 0;
        printf("Range %g to %g", range.minimum, range.maximum);
        if (range.increment_mode == IRIS_INCREMENT_FIXED) printf(", step %g", range.increment);
        putchar('\n');
    } else if (type == IRIS_FEATURE_ENUMERATION) {
        size_t count;
        if (!CHECK(iris_feature_enumeration_count(map, name, &count))) return 0;
        for (size_t i = 0; i < count; ++i) {
            size_t size = 0;
            iris_feature_access_t access;
            if (!CHECK(iris_feature_enumeration_entry(map, name, i, NULL, &size, &access)))
                return 0;
            char *symbol = malloc(size);
            if (!symbol) {
                fputs("Cannot allocate enumeration symbol\n", stderr);
                return 0;
            }
            const int ok = CHECK(iris_feature_enumeration_entry(map, name, i, symbol, &size, &access));
            if (ok) printf("  %s (access %" PRId32 ")\n", symbol, access);
            free(symbol);
            if (!ok) return 0;
        }
    }
    return 1;
}

static int write_value(iris_node_map_t *map, const char *name, iris_feature_type_t type, const char *value)
{
    switch (type) {
    case IRIS_FEATURE_INTEGER: {
        char *end;
        errno = 0;
        const intmax_t integer = strtoimax(value, &end, 10);
        if (errno || end == value || *end || integer < INT64_MIN || integer > INT64_MAX) {
            fputs("Expected a signed 64-bit decimal integer\n", stderr);
            return 0;
        }
        return CHECK(iris_feature_set_integer(map, name, (int64_t)integer));
    }
    case IRIS_FEATURE_STRING:
        return CHECK(iris_feature_set_string(map, name, value));
    case IRIS_FEATURE_ENUMERATION:
        return CHECK(iris_feature_set_enumeration(map, name, value));
    case IRIS_FEATURE_BOOLEAN:
        if (strcmp(value, "0") == 0 || strcmp(value, "1") == 0)
            return CHECK(iris_feature_set_boolean(map, name, (uint32_t)(value[0] - '0')));
        fputs("Use 0 or 1 for Boolean values\n", stderr);
        return 0;
    default:
        // GenApi parses and validates the remaining textual values.
        return CHECK(iris_feature_from_string(map, name, value));
    }
}

static int run(iris_node_map_t *map, const char *name, const char *value, int execute)
{
    iris_feature_info_t info;
    if (!CHECK(iris_feature_info(map, name, &info))) return 0;
    printf("%s has type %" PRId32 " and access %" PRId32 "\n", name, info.type, info.access);

    if (execute) {
        uint32_t done;
        if (!CHECK(iris_feature_execute(map, name)) ||
            !CHECK(iris_feature_command_done(map, name, &done))) return 0;
        printf("%s %s\n", name, done ? "completed" : "pending");
        return 1;
    }

    const int readable = info.access == IRIS_FEATURE_READ_ONLY || info.access == IRIS_FEATURE_READ_WRITE;
    const int writable = info.access == IRIS_FEATURE_WRITE_ONLY || info.access == IRIS_FEATURE_READ_WRITE;
    if (!value && !readable) {
        fprintf(stderr, "%s is not readable in the current selector state\n", name);
        return 0;
    }
    if (value && !writable) {
        fprintf(stderr, "%s is not writable in the current selector state\n", name);
        return 0;
    }
    if (readable && (!print_constraints(map, name, info.type) || !print_value(map, name, info.type)))
        return 0;
    if (value && !write_value(map, name, info.type, value)) return 0;
    return !value || !readable || print_value(map, name, info.type);
}

static int browse(iris_node_map_t *map, const char *category)
{
    iris_feature_list_t *list = NULL;
    if (!CHECK(iris_node_map_browse(map, category, IRIS_CATEGORY_FEATURES, &list))) return 0;
    int ok = 1;
    for (size_t i = 0; i < iris_feature_list_count(list); ++i) {
        iris_feature_entry_t feature;
        if (!CHECK(iris_feature_list_get(list, i, &feature))) {
            ok = 0;
            break;
        }
        printf("%s (%s), type %" PRId32 ", access %" PRId32 ", visibility %" PRId32 "\n",
               feature.name, feature.display_name, feature.info.type,
               feature.info.access, feature.visibility);
        if (*feature.tooltip) printf("  %s\n", feature.tooltip);
    }
    iris_feature_list_close(&list);
    return ok;
}

int main(int argc, char **argv)
{
    int op = 3;
    const char *selector = NULL;
    const char *symbol = NULL;
    if (argc > 5 && strcmp(argv[3], "--select") == 0) {
        selector = argv[4];
        symbol = argv[5];
        op = 6;
    }
    const int get = argc == op + 2 && strcmp(argv[op], "get") == 0;
    const int set = argc == op + 3 && strcmp(argv[op], "set") == 0;
    const int execute = argc == op + 2 && strcmp(argv[op], "execute") == 0;
    const int listing = argc == op + 2 && strcmp(argv[op], "browse") == 0;
    if (!get && !set && !execute && !listing) {
        fprintf(stderr, "Usage: %s CTI SERIAL [--select SELECTOR SYMBOL] "
                        "{browse CATEGORY | get FEATURE | set FEATURE VALUE | execute COMMAND}\n", argv[0]);
        return EXIT_FAILURE;
    }

    iris_context_t *context = NULL;
    iris_device_t *device = NULL;
    iris_node_map_t *remote = NULL;
    const iris_device_selector_t camera = {argv[1], NULL, NULL, argv[2]};
    const iris_device_access_t access = (selector || set || execute) ? IRIS_DEVICE_CONTROL : IRIS_DEVICE_READ_ONLY;
    int ok = CHECK(iris_context_create(&context)) &&
             CHECK(iris_device_open(context, &camera, access, 1000, &device)) &&
             CHECK(iris_device_node_map(device, IRIS_REMOTE_DEVICE, &remote));
    if (ok && selector) ok = CHECK(iris_feature_set_enumeration(remote, selector, symbol));
    if (ok) ok = listing ? browse(remote, argv[op + 1]) :
                 run(remote, argv[op + 1], set ? argv[op + 2] : NULL, execute);
    if (device && !CHECK(iris_device_close(&device))) ok = 0;
    if (context && !CHECK(iris_context_close(&context))) ok = 0;
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
