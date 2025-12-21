# LevelFS - Leveled DAG File System

A custom file system with **leveled folders** instead of traditional directories. Each folder can have multiple **levels** (alternate versions), and levels can be linked across folders, creating a DAG (Directed Acyclic Graph) structure.

## Features

- **Leveled Folders**: Each folder contains multiple levels (versions) rather than just one directory
- **Level Linking**: Share a level between two folders - changes in one appear in the other
- **Permissions**: Read/Write/Execute permissions for files, folders, and levels
- **Symlinks & Hardlinks**: Symbolic and hard link support
- **Journal**: Crash recovery via journaling
- **Auto-Mount**: Scan and auto-mount LevelFS volumes
- **Import/Export**: Transfer files between host system and LevelFS
- **Binary Execution**: Execute binaries directly from LevelFS (`./<file>`)
- **RootFS Auto-Init**: Automatic `/local` and `/data` directory initialization
- **Variable System**: Declare variables with `$var` expansion support
- **PATH-like Execution**: Run executables from `/local` without path prefix
- **Background Tasks**: Run commands in background with `&` suffix
- **Defragmentation**: Built-in disk defragmentation with multiple options
- **Filesystem Check**: Integrity checking via `fsck`
- **Multi-threaded I/O**: Thread pool for parallel file operations
- **Cache System**: Permission and entry caching for performance
- **File Locking**: Thread-safe file operations with lock manager

## Building

```bash
g++ -std=c++17 -static -static-libgcc -static-libstdc++ -pthread src/mount.cpp src/lstream.cpp -o bin/mount.exe

g++ src/fs.cpp -o bin/fs.exe -lole32 -lsetupapi
```

## Usage

### Format a Disk
```bash
./fs format
```

### Mount
```bash
./mount D           # Mount drive D or any drive letter
./mount auto        # Auto-scan for LevelFS volumes
```

## Shell Commands

### Navigation & Listing

| Command | Description |
|---------|-------------|
| `look` | List directory contents |
| `look -d [path]` | Detailed view (size, perms, timestamps) |
| `look <folder>` | List levels of a specific folder |
| `look <folder>:<level>` | List contents of specific folder level |
| `nav <path>` | Navigate to folder |
| `nav .:<level>` | Switch to different level in current folder |
| `nav ..` | Go to parent (remembers root level) |
| `dir-tree` | Display directory tree |
| `current` | Show current path and level |
| `levels` | List all levels in registry |

### File & Folder Operations

| Command | Description |
|---------|-------------|
| `create folder <name>` | Create folder |
| `create file <name.ext>` | Create file (e.g., `readme.txt`) |
| `read <name.ext>` | Read file contents |
| `write <name.ext>` | Text editor for file |
| `write insert <name.ext>` | Insert line at position (arrow keys) |
| `del <name.ext>` | Delete file/symlink |
| `del <folder>:<level>` | Delete a level from a folder |
| `del -r <folder>` | Recursive delete (required for non-empty folders) |
| `move <src> <dst>` | Move/rename files or folders |
| `rename <path> <newname>` | Rename file, folder, or level |

### Permission Management

| Command | Description |
|---------|-------------|
| `perms <+/-rwx> <path>` | Set permissions on file/folder (e.g., `perms +x script.sh`) |
| `perms <+/-rwx> <folder>:<level>` | Set permissions on a level |

### Links

| Command | Description |
|---------|-------------|
| `symlink <target> <link>` | Create symbolic link |
| `hardlink <target> <link>` | Create hard link |
| `link <dir1> <dir2> <level>` | Create shared level between folders (DAG) |

### Level Operations

| Command | Description |
|---------|-------------|
| `level add <folder\|.> <name>` | Add new level to folder |
| `level branch <folder\|.> <parent> <new>` | Branch level from parent |
| `level remove <folder\|.> <name>` | Remove level from folder |
| `level rename <folder\|.> <old> <new>` | Rename a level |
| `mount-level <path> <levelID>` | Mount level by ID at path |

### Import/Export/Execute

| Command | Description |
|---------|-------------|
| `import <host-path> [name.ext]` | Import file from host system to LevelFS |
| `export <name.ext> <host-path>` | Export file from LevelFS to host system |
| `./<name.ext> [args]` | Execute binary from current directory (requires `+x`) |
| `<cmd> [args]` | Auto-execute from `/local` if found (requires `+x`) |

### Variables

| Command | Description |
|---------|-------------|
| `declare <name>=<value>` | Set variable (persisted to `/data/var.dat`) |
| `$varname` | Expand variable in commands |

### Maintenance

| Command | Description |
|---------|-------------|
| `fsck` | Filesystem integrity check |
| `fraginfo` | Show fragmentation analysis |
| `defrag [-nvfr]` | Defragment disk |
| | `-n, --dry-run`: Analyze only, no changes |
| | `-v, --verbose`: Detailed output |
| | `-f, --force`: Force processing |
| | `-r, --recursive`: Include subdirectories |

### Other Commands

| Command | Description |
|---------|-------------|
| `mount <D\|auto>` | Mount drive letter or auto-scan |
| `log <on\|off>` | Toggle disk operation logging |
| `jobs` | List background tasks |
| `<command> &` | Run command in background |
| `help` | Show all commands |
| `exit` | Exit shell |

## Structure Example

```
/master
├── local/
│   └── :master
│       ├── utility.exe
│       └── script.bat
├── data/
│   └── :master
│       └── var.dat
├── Projects/
│   ├── :master
│   │   ├── note.txt
│   │   └── file.html
│   └── :experiment (linked)
│       └── shared.txt
├── bin/
│   └── :master
│       ├── phones.md
│       └── site.html
└── Sys/
    ├── :master
    │   └── people.txt
    └── :experiment (linked)  ←── Same content as Projects:experiment
        └── shared.txt
```

## RootFS Structure

On first mount, LevelFS automatically initializes:
- `/local` - For executable files (searched for command execution)
- `/data` - For system data
  - `/data/var.dat` - Variable storage file

## Architecture

### Disk Layout
- **Superblock**: Filesystem metadata and configuration
- **LAB Pool**: Level Allocation Blocks - track cluster allocation per level
- **LAT Pool**: Level Allocation Table - cluster chain linkage
- **LIT Pool**: Level Index Table - level-to-cluster mapping
- **Level Registry**: Global level metadata
- **Data Clusters**: Actual file and directory content

### Key Components
- `FileSystemShell`: Main shell interface and command processor
- `DiskDevice`: Low-level disk I/O operations
- `Journal`: Crash recovery and transaction logging
- `PermissionResolver`: Permission checking with inheritance
- `EntryReader/EntryWriter`: Directory entry I/O
- `FileLockManager`: Thread-safe file locking
- `ThreadPool`: Parallel I/O operations
- `Defragmenter`: Disk defragmentation engine

## License

Apache License 2.0