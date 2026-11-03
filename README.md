# CS238_Project3
Implementation of a robust memory management system featuring an API reminiscent of the C malloc() function

## Build and run (Linux / WSL)

```
make                       # builds ./cs238
./cs238 --truncate demo.scm   # start fresh; data is stored in demo.scm
```

Options: `--truncate` (clear saved data), `--nocolor`, `--help`.

## Demo steps

1. Start fresh and add words:
   ```
   ./cs238 --truncate demo.scm
   insert apple
   insert apple
   insert banana
   list
   info
   ```
   `info` shows item count, unique words, bytes used, and capacity.

2. Remove a word and check memory is reused:
   ```
   remove banana
   exists apple
   info
   quit
   ```

3. Show persistence: run again **without** `--truncate`.
   ```
   ./cs238 demo.scm
   list
   info
   ```
   The words from the previous run are still there, because the file is
   mapped at the same address every run.

## Commands

`insert <word>`, `remove <word>`, `exists <word>`, `load <file>`,
`list`, `info`, `help`, `quit`

## Tests

```
make test
```

## Valgrind (leak check)

Install once (Ubuntu/WSL): `sudo apt install valgrind`

Run the shell under valgrind, then type commands as usual and `quit`:

```
make
valgrind --leak-check=full --show-leak-kinds=all ./cs238 --truncate demo.scm
```

Or run the whole test suite under valgrind:

```
valgrind --leak-check=full --error-exitcode=1 ./tests/test_extra
```

Expected: `All heap blocks were freed -- no leaks are possible` and
`ERROR SUMMARY: 0 errors`.

Note: valgrind only tracks regular `malloc` memory. The SCM region is an
`mmap`ed file, so check it with `info` (utilized bytes should return to 0
after removing everything).
