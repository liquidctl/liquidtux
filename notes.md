# Notes on the Kraken X.2 and Smart Device (V1)/Grid+ V3 drivers

## Kraken X.2

Device has three sensors: coolant temperature, fan(s) speed, and pump speed.

Good labels: Coolant, Fans, Pump?  Probably yes, they are cohesive with
coretemp.

### Stuff to do

#### Critical

- [x] remove data races
- [ ] check for other UB
- [ ] check for suspend issues
- [x] fix parsing of the cooling temperature (good enough for now)

### Medium

- [x] no `hwmon_device_unregister` in `*_remove`?

#### Kernel/hwmon coding style

- [ ] `sizeof(*data_value)` or `sizeof(struct data_type)`?
- [x] `ldata` or `priv`? (the latter is a common idiom in hwmon)
- [x] `devm_hwmon_device_register_with_info` or
  `hwmon_device_register_with_info`? (use the latter is if `.remove` is needed)
- [x] `kraken2_device_data` or `kraken2_data` or `kraken2_priv_data`? (the latter is clearer)

### Optimizations

- [ ] try to use `hid_driver.report_table` to only do work for report ID 0x04
  (but for some reason `hid_report_id` works with report *types*)

#### Low severity

- [x] decide on good names for the modules and the corresponding hwmon devices
- [x] decide on good labels for the sensors
- [x] adjust copyright header


## Smart Device (V1)/Grid+ V3

Device has several sensors for each fan channel (speed, voltage, current,
control mode), plus a noise pressure level sensor.


## Naming modules and hwmon devices

Current:
- Kraken X.2 -> nzxt-kraken2
- Smart Device (V1)/Grid+ V3 -> nzxt-sd1

Future:
- Kraken X.3 -> nzxt-krakenx3
- Kraken Z.3 -> nzxt-krakenz3
- Smart Device (V2)/RGB & Fan Controller -> nzxt-sd2


## ...

`memcpy within spin_lock_irqsave` (outdated):

```
0000000000000130 <kraken_raw_event>:
 130:   e8 00 00 00 00          call   135 <kraken_raw_event+0x5>
 135:   41 55                   push   r13
 137:   41 54                   push   r12
 139:   45 31 e4                xor    r12d,r12d
 13c:   55                      push   rbp
 13d:   53                      push   rbx
 13e:   83 7e 20 04             cmp    DWORD PTR [rsi+0x20],0x4
 142:   75 2d                   jne    171 <kraken_raw_event+0x41>
 144:   83 f9 07                cmp    ecx,0x7
 147:   7e 32                   jle    17b <kraken_raw_event+0x4b>
 149:   4c 8b af 48 19 00 00    mov    r13,QWORD PTR [rdi+0x1948]
 150:   48 89 d3                mov    rbx,rdx
 153:   49 8d 6d 10             lea    rbp,[r13+0x10]
 157:   48 89 ef                mov    rdi,rbp
 15a:   e8 00 00 00 00          call   15f <kraken_raw_event+0x2f>
 15f:   48 89 ef                mov    rdi,rbp
 162:   48 89 c6                mov    rsi,rax
 165:   48 8b 03                mov    rax,QWORD PTR [rbx]
 168:   49 89 45 14             mov    QWORD PTR [r13+0x14],rax
 16c:   e8 00 00 00 00          call   171 <kraken_raw_event+0x41>
 171:   5b                      pop    rbx
 172:   44 89 e0                mov    eax,r12d
 175:   5d                      pop    rbp
 176:   41 5c                   pop    r12
 178:   41 5d                   pop    r13
 17a:   c3                      ret    
 17b:   41 bc c3 ff ff ff       mov    r12d,0xffffffc3
 181:   eb ee                   jmp    171 <kraken_raw_event+0x41>
 ```
