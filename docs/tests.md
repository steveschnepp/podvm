# Tests

> Test suite for podvm POC.

---

## Running Tests

```bash
./tests/run_tests.sh
```

## Test Structure

```
tests/
├── Makefile              # Build tests
├── run_tests.sh          # Test runner
├── test_config.c         # Config parsing tests
├── test_host_args.c      # Host argument tests
├── test_vsock.c          # Vsock communication tests
├── test_kernel_config.c  # Kernel config validation
└── test_integration.c    # Integration tests
```

## Unit Tests

### test_config.c
Tests config parsing functions:
- `kv_find()` - Find key=value pairs
- `kv_copy_value()` - Copy values
- `csv_parse()` - Parse comma-separated values
- `parse_uid_gid()` - Parse uid:gid strings
- `parse_cmdline()` - Full cmdline parsing

### test_host_args.c
Tests host argument parsing:
- `-m` memory flag
- `-c` CPU flag
- `-it` interactive flag
- Config string building
- Memory backend building

### test_vsock.c
Tests vsock communication:
- Socket creation
- Address construction
- Server/client communication

### test_kernel_config.c
Validates kernel/config:
- Required options present
- Forbidden options absent
- Security options enabled

## Integration Tests

### test_integration.c
Checks build and dependencies:
- Host binary builds
- Guest init builds
- Initramfs builds
- QEMU available
- KVM available
- virtiofsd available

## Test Results

Current status:
- 14 PASS
- 2 WARN (missing musl-gcc, virtiofsd)
- 0 FAIL

## Adding Tests

1. Create `tests/test_xxx.c`
2. Add to `tests/Makefile`
3. Add to `tests/run_tests.sh`
