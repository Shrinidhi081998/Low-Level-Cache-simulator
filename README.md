# L1 Cache Simulator – Run Instructions

## Prerequisites
- GCC must be installed and available in the system PATH.

---

## Project Structure

```
src/            -> Source files
includes/       -> Header files
inputs/tests/   -> Input trace files
app/            -> Executable output (cache_sim.exe)
outputs/        -> Output logs
```

Executable generated:

```
app/cache_sim.exe
```

Output log file:

```
outputs/output_traces.txt
```

---

## How to Run

1. Open **PowerShell** or **Command Prompt** in the project folder.
2. Run the batch file:

```
run.bat
```

This will:
- Compile the project
- Create the executable `app/cache_sim.exe`
- Run the default trace file

---

## Default Settings in `run.bat`

Trace file:
```
inputs/tests/scenario_s2_rw_snoop_stepdump.txt
```

Mode:
```
0
```

---

## Running with Different Modes

Run with Mode 1:

```
run.bat 1
```

### Mode Values

| Mode | Description |
|------|-------------|
| 0 | Summary + cache dump output |
| 1 | Mode 0 + L2 communication logs |

---

## Running with Different Trace Files

```
run.bat 1 inputs/tests/t01_data_read_miss_hit.txt
```

---

## Useful Examples

```
run.bat 0 inputs/tests/t10_full_opcode_smoke.txt
run.bat 1 inputs/tests/scenario_s1_fill_set_stepdump.txt
```

---

## Debug Build

To enable debug compile mode, edit `run.bat` and set:

```
set DEBUG=1
```

---

## Generated Outputs

Executable:
```
app/cache_sim.exe
```

Output logs:
```
outputs/output_traces.txt
```

Logs are appended to the output file each time the simulator runs.
