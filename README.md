# Radare2 MCP Server

<img width="400" alt="Screenshot_2025-03-22_at_5 34 47_PM" src="https://github.com/user-attachments/assets/5322c3fc-fc07-4770-96a3-5a6d82d439c2" />
<img width="400" alt="Screenshot_2025-03-22_at_5 36 17_PM" src="https://github.com/user-attachments/assets/132a1de0-6978-4202-8dce-aa3d60551b9a" />

A MCP server for using radare2 with AI assistants like Claude.

## Features

This implementation provides a simple MCP server that:

- Uses a direct stdin/stdout communication model
- Provides basic tool capabilities
- Allows seamless binary analysis with radare2
- Direct integration of radare2 with AI assistants
- File exploration and inspection

## Installation

The simplest way to install the package is by using `r2pm`:

```bash
$ r2pm -Uci r2mcp
```

The r2mcp executable will be copied into the r2pm's bindir in your home, but this binary is not suposed to be executed from the shell because it will only work when launched from the MCP service handler of your language model of choice.

## Configuration

In the Claude Desktop app press `CMD + ,` to open the Developer settings to edit the configuration file and restart the client after editing the json file explained below

### Claude Desktop Integration

To use this with Claude Desktop, update your Claude configuration file:

1. Locate your Claude Desktop configuration file:

   - macOS: `~/Library/Application Support/Claude/claude_desktop_config.json`
   - Windows: `%APPDATA%\Claude\claude_desktop_config.json`
   - Linux: `~/.config/Claude/claude_desktop_config.json`

2. Add the following to your configuration file:

```json
{
  "mcpServers": {
    "radare2": {
      "command": "r2pm",
      "args": ["-r", "r2mcp"]
    }
  }
}
```

## Docker

Alternatively, you can use Docker to run r2mcp.

```bash
docker build -t r2mcp .
```

Then, update your Claude Desktop configuration file to use the Docker image:

```json
{
  "mcpServers": {
    "radare2": {
      "command": "docker",
      "args": ["run", "--rm", "-i", "-v", "/tmp/data:/data", "r2mcp"]
    }
  }
}
```


