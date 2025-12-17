# LEVELFS

A custom made Leveled DAG file system that uses leveled Folders instead of normal folders. Each folder can hold to as many levels as you want, Levels are basically alternate versions of that folder, and you can link a level to another folder, creating a link between 2 folders.

** Structure **
/master
    |   
    \ Local(folder)
        |
        \ master (level)
        |    |
        |    \ note.txt
        |    \ file.html
        |
        \ exp (Linked level)
            |
            \ file.txt
        
    \ bin (folder)
        |
        \ master (level)
            |
            \ phones.md
            \ site.html
    \ Sys (folder)
        |
        \ master (level)
        |    |
        |    \ people.txt
        |    \ perms.html
        |
        \ exp (Linked level)
            |
            \ file.txt

# Implementations:

- [ ] IDK