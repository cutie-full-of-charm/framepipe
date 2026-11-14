## Logs folder
```sh
  mkdir -p logs/sanitizers
```

## Build with thread sanitizer (tsan) 
```sh
cmake --preset linux-tsan
cmake --build --preset linux-tsan
```


## Run with tsan preload
```sh
TSAN_LIB="$(clang++ -print-file-name=libclang_rt.tsan-x86_64.so)"

env \
  LD_PRELOAD="$TSAN_LIB" \
  LD_LIBRARY_PATH="$(dirname "$TSAN_LIB")${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1:log_path=logs/sanitizers/tsan" \
  .venv/bin/python <SCRIPT> <ARGS> 
```

## Build with address sanitizer + undefined behavior sanitizers (asan+ubsan)
```sh
cmake --preset linux-asan
cmake --build --preset linux-asan
```

## Run with asan+ubsan preload
```sh
ASAN_LIB="$(clang++ -print-file-name=libclang_rt.asan-x86_64.so)"

env \
  LD_PRELOAD="$ASAN_LIB" \
  LD_LIBRARY_PATH="$(dirname "$ASAN_LIB")${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  ASAN_OPTIONS="detect_leaks=1:protect_shadow_gap=0:halt_on_error=1:log_path=logs/sanitizers/asan" \
  LSAN_OPTIONS="suppressions=$(pwd)/tools/sanitizers/lsan.supp:print_suppressions=1" \
  UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1:log_path=logs/sanitizers/ubsan" \
  .venv/bin/python <SCRIPT> <ARGS> 
```
