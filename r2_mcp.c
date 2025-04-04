#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdarg.h>
#include <r_core.h>
#include <r_util/r_json.h>
#include <r_util/r_print.h>
#include <signal.h>
#include <sys/select.h>
#include <errno.h>
#include <fcntl.h>

static const char *r_json_get_str(const RJson *json, const char *key) {
    if (!json || !key) {
        return NULL;
    }
    
    const RJson *field = r_json_get(json, key);
    if (!field || field->type != R_JSON_STRING) {
        return NULL;
    }
    
    return field->str_value;
}

#define PORT 3000
#define BUFFER_SIZE 65536
#define READ_CHUNK_SIZE 4096

#define LATEST_PROTOCOL_VERSION "2024-11-05"

typedef struct {
    const char *name;
    const char *version;
} ServerInfo;

typedef struct {
    bool logging;
    bool tools;
} ServerCapabilities;

typedef struct {
    ServerInfo info;
    ServerCapabilities capabilities;
    const char *instructions;
    bool initialized;
    RJson *client_capabilities;
    RJson *client_info;
} ServerState;

static RCore *r_core = NULL;
static bool file_opened = false;
static char current_file[1024] = {0};
static volatile sig_atomic_t running = 1;
static bool is_direct_mode = false;
static ServerState server_state = {
    .info = {
        .name = "Radare2 MCP Connector",
        .version = "1.0.0"
    },
    .capabilities = {
        .logging = true,
        .tools = true
    },
    .instructions = "Use this server to analyze binaries with radare2",
    .initialized = false,
    .client_capabilities = NULL,
    .client_info = NULL
};

// Forward declarations
static void process_mcp_message(const char *msg);
static void direct_mode_loop(void);

#define JSON_RPC_VERSION "2.0"
#define MCP_VERSION "2024-11-05"

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} ReadBuffer;

ReadBuffer* read_buffer_new() {
    ReadBuffer *buf = malloc(sizeof(ReadBuffer));
    buf->data = malloc(BUFFER_SIZE);
    buf->size = 0;
    buf->capacity = BUFFER_SIZE;
    return buf;
}

void read_buffer_append(ReadBuffer *buf, const char *data, size_t len) {
    if (buf->size + len > buf->capacity) {
        size_t new_capacity = buf->capacity * 2;
        char *new_data = realloc(buf->data, new_capacity);
        if (!new_data) {
            fprintf(stderr, "Failed to resize buffer\n");
            return;
        }
        buf->data = new_data;
        buf->capacity = new_capacity;
    }
    memcpy(buf->data + buf->size, data, len);
    buf->size += len;
}

char* read_buffer_get_message(ReadBuffer *buf) {
    char *newline = memchr(buf->data, '\n', buf->size);
    if (!newline) return NULL;
    
    size_t msg_len = newline - buf->data;
    char *msg = malloc(msg_len + 1);
    memcpy(msg, buf->data, msg_len);
    msg[msg_len] = '\0';
    
    size_t remaining = buf->size - (msg_len + 1);
    if (remaining > 0) {
        memmove(buf->data, newline + 1, remaining);
    }
    buf->size = remaining;
    
    return msg;
}

void read_buffer_free(ReadBuffer *buf) {
    free(buf->data);
    free(buf);
}

static char *get_capabilities();
static char *handle_initialize(RJson *params);
static char *handle_list_tools(RJson *params);
static char *handle_call_tool(RJson *params);
static char *format_string(const char *format, ...);
static char *format_string(const char *format, ...) {
    char buffer[4096];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return strdup(buffer);
}

static bool init_r2(void) {
    r_core = r_core_new();
    if (!r_core) {
        fprintf(stderr, "Failed to initialize radare2 core\n");
        return false;
    }
    
    r_config_set_i(r_core->config, "scr.color", 0);
    
    fprintf(stderr, "Radare2 core initialized\n");
    return true;
}

void cleanup_r2() {
    if (r_core) {
        r_core_free(r_core);
        r_core = NULL;
        file_opened = false;
        memset(current_file, 0, sizeof(current_file));
    }
}

bool r2_open_file(const char *filepath) {
    fprintf(stderr, "Attempting to open file: %s\n", filepath);
    
    if (!r_core && !init_r2()) {
        fprintf(stderr, "Failed to initialize r2 core\n");
        return false;
    }
    
    if (file_opened) {
        fprintf(stderr, "Closing previously opened file: %s\n", current_file);
        r_core_cmd0(r_core, "o-*");
        file_opened = false;
        memset(current_file, 0, sizeof(current_file));
    }
    
    r_core_cmd0(r_core, "e bin.relocs.apply=true");
    r_core_cmd0(r_core, "e bin.cache=true");
    
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "o %s", filepath);
    fprintf(stderr, "Running r2 command: %s\n", cmd);
    
    char *result = r_core_cmd_str(r_core, cmd);
    bool success = (result && strlen(result) > 0);
    free(result);
    
    if (!success) {
        fprintf(stderr, "Trying alternative method to open file...\n");
        RIODesc *fd = r_core_file_open(r_core, filepath, R_PERM_R, 0);
        if (fd) {
            r_core_bin_load(r_core, filepath, 0);
            fprintf(stderr, "File opened using r_core_file_open\n");
            success = true;
        } else {
            fprintf(stderr, "Failed to open file: %s\n", filepath);
            return false;
        }
    }
    
    fprintf(stderr, "Loading binary information\n");
    r_core_cmd0(r_core, "ob");
    
    strncpy(current_file, filepath, sizeof(current_file) - 1);
    file_opened = true;
    fprintf(stderr, "File opened successfully: %s\n", filepath);
    
    return true;
}

char *r2_cmd(const char *cmd) {
    if (!r_core || !file_opened) {
        return strdup("Error: No file is open");
    }
    return r_core_cmd_str(r_core, cmd);
}

bool r2_analyze(const char *level) {
    if (!r_core || !file_opened) return false;
    
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "%s", level);
    r_core_cmd0(r_core, cmd);
    return true;
}

static void signal_handler(int signum) {
    const char msg[] = "\nInterrupt received, shutting down...\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
    
    running = 0;
    
    signal(signum, SIG_DFL);
}

static bool check_client_capability(const char *capability) {
    if (!server_state.client_capabilities) return false;
    RJson *cap = (RJson *)r_json_get(server_state.client_capabilities, capability);
    return cap != NULL;
}

static bool check_server_capability(const char *capability) {
    if (!strcmp(capability, "logging")) return server_state.capabilities.logging;
    if (!strcmp(capability, "tools")) return server_state.capabilities.tools;
    return false;
}

static bool assert_capability_for_method(const char *method, char **error) {
    if (!strcmp(method, "sampling/createMessage")) {
        if (!check_client_capability("sampling")) {
            *error = strdup("Client does not support sampling");
            return false;
        }
    } else if (!strcmp(method, "roots/list")) {
        if (!check_client_capability("roots")) {
            *error = strdup("Client does not support listing roots");
            return false;
        }
    }
    return true;
}

static bool assert_request_handler_capability(const char *method, char **error) {
    if (!strcmp(method, "sampling/createMessage")) {
        if (!check_server_capability("sampling")) {
            *error = strdup("Server does not support sampling");
            return false;
        }
    } else if (!strcmp(method, "logging/setLevel")) {
        if (!check_server_capability("logging")) {
            *error = strdup("Server does not support logging");
            return false;
        }
    } else if (!strncmp(method, "prompts/", 8)) {
        if (!check_server_capability("prompts")) {
            *error = strdup("Server does not support prompts");
            return false;
        }
    } else if (!strncmp(method, "tools/", 6)) {
        if (!check_server_capability("tools")) {
            *error = strdup("Server does not support tools");
            return false;
        }
    }
    return true;
}

// Helper function to create JSON-RPC error responses
static char *create_error_response(int code, const char *message, const char *id, const char *uri) {
    PJ *pj = pj_new();
    pj_o(pj);
    pj_ks(pj, "jsonrpc", "2.0");
    if (id) pj_ks(pj, "id", id);
    pj_k(pj, "error");
    pj_o(pj);
    pj_ki(pj, "code", code);
    pj_ks(pj, "message", message);
    if (uri) {
        pj_k(pj, "data");
        pj_o(pj);
        pj_ks(pj, "uri", uri);
        pj_end(pj);
    }
    pj_end(pj);
    pj_end(pj);
    return pj_drain(pj);
}

// Helper function to create a successful JSON-RPC response
static char *create_success_response(const char *result, const char *id) {
    PJ *pj = pj_new();
    pj_o(pj);
    pj_ks(pj, "jsonrpc", "2.0");
    if (id) pj_ks(pj, "id", id);
    pj_k(pj, "result");
    if (result) {
        pj_raw(pj, result);
    } else {
        pj_null(pj);
    }
    pj_end(pj);
    return pj_drain(pj);
}

// Helper function to create tool error responses with specific format
static char *create_tool_error_response(const char *error_message) {
    PJ *pj = pj_new();
    pj_o(pj);
    pj_k(pj, "content");
    pj_a(pj);
    pj_o(pj);
    pj_ks(pj, "type", "text");
    pj_ks(pj, "text", error_message);
    pj_end(pj);
    pj_end(pj);
    pj_kb(pj, "isError", true);
    pj_end(pj);
    return pj_drain(pj);
}

// Helper function to create a simple text tool result
static char *create_tool_text_response(const char *text) {
    PJ *pj = pj_new();
    pj_o(pj);
    pj_k(pj, "content");
    pj_a(pj);
    pj_o(pj);
    pj_ks(pj, "type", "text");
    pj_ks(pj, "text", text);
    pj_end(pj);
    pj_end(pj);
    pj_end(pj);
    return pj_drain(pj);
}

static char *handle_mcp_request(const char *method, RJson *params, const char *id) {
    char *error = NULL;
    char *result = NULL;

    if (!assert_capability_for_method(method, &error) || !assert_request_handler_capability(method, &error)) {
        char *response = create_error_response(-32601, error, id, NULL);
        free(error);
        return response;
    }

    if (!strcmp(method, "initialize")) {
        result = handle_initialize(params);
    } else if (!strcmp(method, "ping")) {
        result = strdup("{}");
    } else if (!strcmp(method, "resources/templates/list")) {
        return create_error_response(-32601, "Method not implemented: templates are not supported", id, NULL);
    } else if (!strcmp(method, "resources/list")) {
        return create_error_response(-32601, "Method not implemented: resources are not supported", id, NULL);
    } else if (!strcmp(method, "resources/read") || !strcmp(method, "resource/read")) {
        return create_error_response(-32601, "Method not implemented: resources are not supported", id, NULL);
    } else if (!strcmp(method, "resources/subscribe") || !strcmp(method, "resource/subscribe")) {
        return create_error_response(-32601, "Method not implemented: subscriptions are not supported", id, NULL);
    } else if (!strcmp(method, "tools/list") || !strcmp(method, "tool/list")) {
        result = handle_list_tools(params);
    } else if (!strcmp(method, "tools/call") || !strcmp(method, "tool/call")) {
        result = handle_call_tool(params);
    } else {
        return create_error_response(-32601, "Unknown method", id, NULL);
    }

    char *response = create_success_response(result, id);
    free(result);
    return response;
}

static char *handle_initialize(RJson *params) {
    if (server_state.client_capabilities) r_json_free(server_state.client_capabilities);
    if (server_state.client_info) r_json_free(server_state.client_info);
    
    server_state.client_capabilities = (RJson *)r_json_get(params, "capabilities");
    server_state.client_info = (RJson *)r_json_get(params, "clientInfo");
    
    PJ *pj = pj_new();
    pj_o(pj);
    pj_ks(pj, "protocolVersion", LATEST_PROTOCOL_VERSION);
    pj_k(pj, "serverInfo");
    pj_o(pj);
    pj_ks(pj, "name", server_state.info.name);
    pj_ks(pj, "version", server_state.info.version);
    pj_end(pj);
    pj_k(pj, "capabilities");
    pj_raw(pj, get_capabilities());
    if (server_state.instructions) {
        pj_ks(pj, "instructions", server_state.instructions);
    }
    pj_end(pj);
    
    server_state.initialized = true;
    return pj_drain(pj);
}

static char *get_capabilities() {
    PJ *pj = pj_new();
    pj_o(pj);
    
    pj_k(pj, "tools");
    pj_o(pj);
    pj_end(pj);
    
    pj_end(pj);
    return pj_drain(pj);
}

static char *handle_list_tools(RJson *params) {
    // Add pagination support
    const char *cursor = r_json_get_str(params, "cursor");
    int page_size = 10; // Default page size
    int start_index = 0;
    
    // Parse cursor if provided
    if (cursor) {
        start_index = atoi(cursor);
        if (start_index < 0) start_index = 0;
    }
    
    // Use more straightforward JSON construction for tools list
    char *result = NULL;
    size_t result_size = 0;
    FILE *stream = open_memstream(&result, &result_size);
    
    if (!stream) {
        fprintf(stderr, "Failed to create memory stream\n");
        return strdup("{\"tools\":[]}");
    }
    
    fprintf(stream, "{\"tools\":[");
    
    // Define our tools with their descriptions and schemas
    // Format: {name, description, schema_definition}
    const char *tools[][3] = {
        {
            "openFile", 
            "Open a file for analysis", 
            "{\"type\":\"object\",\"properties\":{\"filePath\":{\"type\":\"string\",\"description\":\"Path to the file to open\"}},\"required\":[\"filePath\"]}"
        },
        {
            "closeFile", 
            "Close the currently open file", 
            "{\"type\":\"object\",\"properties\":{}}"
        },
        {
            "runCommand", 
            "Run a radare2 command and get the output", 
            "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"Command to execute\"}},\"required\":[\"command\"]}"
        },
        {
            "analyze", 
            "Run analysis on the current file", 
            "{\"type\":\"object\",\"properties\":{\"level\":{\"type\":\"string\",\"description\":\"Analysis level (a, aa, aaa, aaaa)\"}},\"required\":[]}"
        },
        {
            "disassemble", 
            "Disassemble instructions at a given address", 
            "{\"type\":\"object\",\"properties\":{\"address\":{\"type\":\"string\",\"description\":\"Address to start disassembly\"},\"numInstructions\":{\"type\":\"integer\",\"description\":\"Number of instructions to disassemble\"}},\"required\":[\"address\"]}"
        }
    };
    
    int total_tools = sizeof(tools) / sizeof(tools[0]);
    int end_index = start_index + page_size;
    if (end_index > total_tools) end_index = total_tools;
    
    // Add tools for this page
    for (int i = start_index; i < end_index; i++) {
        if (i > start_index) {
            fprintf(stream, ",");
        }
        
        fprintf(stream, "{\"name\":\"%s\",\"description\":\"%s\",\"inputSchema\":%s}",
                tools[i][0], tools[i][1], tools[i][2]);
    }
    
    fprintf(stream, "]");
    
    // Add nextCursor if there are more tools
    if (end_index < total_tools) {
        char next_cursor[16];
        snprintf(next_cursor, sizeof(next_cursor), "%d", end_index);
        fprintf(stream, ",\"nextCursor\":\"%s\"", next_cursor);
    }
    
    fprintf(stream, "}");
    
    fclose(stream);
    
    // Log the generated JSON for debugging
    fprintf(stderr, "Generated JSON: %s\n", result);
    
    return result;
}


static char *handle_call_tool(RJson *params) {
    const char *tool_name = r_json_get_str(params, "name");
    
    if (!tool_name) {
        return create_error_response(-32602, "Missing required parameter: name", NULL, NULL);
    }
    
    RJson *tool_args = (RJson *)r_json_get(params, "arguments");
    
    // Handle openFile tool
    if (!strcmp(tool_name, "openFile")) {
        const char *filepath = r_json_get_str(tool_args, "filePath");
        if (!filepath) {
            return create_error_response(-32602, "Missing required parameter: filePath", NULL, NULL);
        }
        
        bool success = r2_open_file(filepath);
        return create_tool_text_response(success ? "File opened successfully." : "Failed to open file.");
    }
    
    // Handle closeFile tool
    if (!strcmp(tool_name, "closeFile")) {
        if (!file_opened) {
            return create_tool_text_response("No file was open.");
        }
        
        char filepath_copy[1024];
        strncpy(filepath_copy, current_file, sizeof(filepath_copy) - 1);
        if (r_core) {
            r_core_cmd0(r_core, "o-*");
            file_opened = false;
            memset(current_file, 0, sizeof(current_file));
        }
        
        return create_tool_text_response("File closed successfully.");
    }
    
    // For all other tools, ensure a file is open
    if (!file_opened && (
        !strcmp(tool_name, "runCommand") || 
        !strcmp(tool_name, "analyze") || 
        !strcmp(tool_name, "disassemble")
    )) {
        return create_tool_error_response("No file is currently open. Please open a file first.");
    }
    
    // Handle runCommand tool
    if (!strcmp(tool_name, "runCommand")) {
        const char *command = r_json_get_str(tool_args, "command");
        if (!command) {
            return create_error_response(-32602, "Missing required parameter: command", NULL, NULL);
        }
        
        char *result = r2_cmd(command);
        char *response = create_tool_text_response(result);
        free(result);
        return response;
    }
    
    // Handle analyze tool
    if (!strcmp(tool_name, "analyze")) {
        const char *level = r_json_get_str(tool_args, "level");
        if (!level) level = "aaa";
        
        r2_analyze(level);
        char *result = r2_cmd("afl");
        char *text = format_string("Analysis completed with level %s.\n\n%s", level, result);
        char *response = create_tool_text_response(text);
        free(result);
        free(text);
        return response;
    }
    
    // Handle disassemble tool
    if (!strcmp(tool_name, "disassemble")) {
        const char *address = r_json_get_str(tool_args, "address");
        if (!address) {
            return create_error_response(-32602, "Missing required parameter: address", NULL, NULL);
        }
        
        // Use const_cast pattern
        RJson *num_instr_json = (RJson *)r_json_get(tool_args, "numInstructions");
        int num_instructions = 10;
        if (num_instr_json && num_instr_json->type == R_JSON_INTEGER) {
            num_instructions = (int)num_instr_json->num.u_value;
        }
        
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "pd %d @ %s", num_instructions, address);
        char *disasm = r2_cmd(cmd);
        char *response = create_tool_text_response(disasm);
        free(disasm);
        return response;
    }
    
    return create_error_response(-32602, format_string("Unknown tool: %s", tool_name), NULL, NULL);
}

// Added back the direct_mode_loop implementation
static void process_mcp_message(const char *msg) {
    RJson *request = r_json_parse((char *)msg);
    if (!request) {
        fprintf(stderr, "Invalid JSON\n");
        return;
    }
    
    const char *method = r_json_get_str(request, "method");
    RJson *params = (RJson *)r_json_get(request, "params");
    RJson *id_json = (RJson *)r_json_get(request, "id");
    
    if (!method) {
        fprintf(stderr, "Invalid JSON-RPC message: missing method\n");
        r_json_free(request);
        return;
    }
    
    if (id_json) {
        const char *id = NULL;
        char id_buf[32] = {0};
        if (id_json->type == R_JSON_STRING) {
            id = id_json->str_value;
        } else if (id_json->type == R_JSON_INTEGER) {
            snprintf(id_buf, sizeof(id_buf), "%lld", (long long)id_json->num.u_value);
            id = id_buf;
        }
        
        char *response = handle_mcp_request(method, params, id);
        
        // Write directly to stdout to ensure no extra output
        write(STDOUT_FILENO, response, strlen(response));
        write(STDOUT_FILENO, "\n", 1);
        fflush(stdout);
        
        free(response);
    } else {
        // We don't handle notifications anymore
        fprintf(stderr, "Ignoring notification: %s\n", method);
    }
    
    r_json_free(request);
}

static void direct_mode_loop(void) {
    fprintf(stderr, "Running in MCP direct mode (stdin/stdout)\n");
    
    ReadBuffer *buffer = read_buffer_new();
    char chunk[READ_CHUNK_SIZE];
    
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    
    struct timeval tv;
    fd_set readfds;
    
    while (running) {
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        
        int ret = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &tv);
        
        if (ret < 0) {
            if (errno != EINTR) {
                fprintf(stderr, "Select error: %s\n", strerror(errno));
                break;
            }
            continue;
        }
        
        if (ret == 0) {
            if (write(STDOUT_FILENO, "", 0) < 0) {
                fprintf(stderr, "Client disconnected (stdout closed)\n");
                break;
            }
            continue;
        }
        
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            ssize_t bytes_read = read(STDIN_FILENO, chunk, READ_CHUNK_SIZE);
            
            if (bytes_read > 0) {
                read_buffer_append(buffer, chunk, bytes_read);
                char *msg;
                while ((msg = read_buffer_get_message(buffer)) != NULL) {
                    process_mcp_message(msg);
                    free(msg);
                }
            } else if (bytes_read == 0) {
                fprintf(stderr, "End of input stream\n");
                break;
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    fprintf(stderr, "Error reading from stdin: %s\n", strerror(errno));
                    break;
                }
            }
        }
    }
    
    read_buffer_free(buffer);
    fprintf(stderr, "Direct mode loop terminated\n");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    
    sa.sa_flags = 0;
    
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    
    signal(SIGPIPE, SIG_IGN);

    
    if (!init_r2()) {
        fprintf(stderr, "Failed to initialize radare2\n");
        return 1;
    }
    
    if (!isatty(STDIN_FILENO)) {
        is_direct_mode = true;
        direct_mode_loop();
        cleanup_r2();
        fprintf(stderr, "MCP direct mode terminated gracefully\n");
        return 0;
    }
    
    cleanup_r2();
    return 0;
}