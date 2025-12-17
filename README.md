# LevelFS - Leveled DAG File System

A custom file system with **leveled folders** instead of traditional directories. Each folder can have multiple **levels** (alternate versions), and levels can be linked across folders, creating a DAG (Directed Acyclic Graph) structure.

## Features

- **Leveled Folders**: Each folder contains multiple levels (versions) rather than just one directory
- **Level Linking**: Share a level between two folders - changes in one appear in the other
- **Permissions**: Read/Write/Execute permissions for files, folders, and levels
- **Symlinks & Hardlinks**: Symbolic and hard link support
- **Journal**: Crash recovery via journaling
- **Auto-Mount**: Scan and auto-mount LevelFS volumes

## Building

```bash
g++ src/fs.cpp -o bin/fs.exe
g++ src/mount.cpp -o bin/mount.exe
```

## Usage

### Format a Disk
```bash
sudo ./fs format    # Interactive disk selection
```

### Mount
```bash
./mount D           # Mount drive D or any drive letter
./mount auto        # Auto-scan for LevelFS volumes
```

## Shell Commands

| Command | Description |
|---------|-------------|
| `look` | List directory contents |
| `look -d` | Detailed view (shows virtual `.` and `..`) |
| `look folder:level` | List contents of specific folder level |
| `nav <path>` | Navigate to folder |
| `nav .:level` | Switch to different level in current folder |
| `nav ..` | Go to parent (remembers root level) |
| `dir-tree` | Display directory tree (prompts if multiple root levels) |
| `create folder <name>` | Create folder |
| `create file <name> [ext]` | Create file |
| `del <path>` | Delete file/symlink |
| `del folder:level` | Delete a level from a folder |
| `del -r <folder>` | Recursive delete (required for non-empty folders) |
| `move <src> <dst>` | Move/rename files or folders |
| `perms <+/-rwx> <path>` | Set permissions on file/folder |
| `perms <+/-rwx> folder:level` | Set permissions on a level |
| `symlink <target> <link>` | Create symbolic link |
| `hardlink <target> <link>` | Create hard link |
| `link <dir1> <dir2> <level>` | Create shared level between folders |
| `level add <folder> <name>` | Add level to folder |
| `level branch <folder> <parent> <new>` | Branch level from parent |
| `level remove <folder> <name>` | Remove level |
| `levels` | List all levels in registry |
| `current` | Show current path and level |
| `read <file>` | Read file contents |
| `write <file>` | Text editor for file |
| `fsck` | Filesystem integrity check |
| `defrag` | Defragment disk |

## Structure Example

```
/master
├── Local/
│   ├── :master
│   │   ├── note.txt
│   │   └── file.html
│   └── :exp (linked)
│       └── shared.txt
├── bin/
│   └── :master
│       ├── phones.md
│       └── site.html
└── Sys/
    ├── :master
    │   └── people.txt
    └── :exp (linked)  ←── Same content as Local/:exp
        └── shared.txt
```

## License

MIT License