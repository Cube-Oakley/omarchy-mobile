# USF host protocol on the Pixel 7 Pro (GS201 / cheetah) AoC channel

Recovered by static analysis of the stock vendor libraries in this directory
(`lib64/libusf.so`, `lib64/sensors.usf.so`, `lib64/libaoc.so`, `bin/usf_stats`).
Nothing here was tested against a phone.

Confidence tags used below:

* **[V addr]**: read directly from the disassembly at that address in
  `libusf.so` (or `HAL addr` for `sensors.usf.so`, `aoc addr` for `libaoc.so`).
* **[I]**: inferred (from names, stat-table labels, default values or how a
  caller uses the value), so it is plausible but unproven.

`libusf.so` contains both the client side (used by the HAL) and the
server side (`UsfServerMgr`, `UsfSensor`, `UsfTimeServer`, ...). The AoC
firmware runs the same USF code base, so the AoC's server behaviour is taken
from the AP-side copy of those servers. That is **[I]** for the AoC as a
whole, but each individual server function cited was read directly.

Scratch tools are in `work/`: `elfutil.py` (ELF, APS2/RELR relocations,
annotated objdump), `vtable.py`, `fbops.py` (condenses inlined FlatBufferBuilder
code) and `vsum.py` (summarises inlined FlatBuffers verifiers).

---

## 0. Summary

* **Framing.** Each `read()`/`write()` on `/dev/acd-com.google.usf` carries
  exactly one **bare FlatBuffer**. There is no length, magic, CRC or sequence
  header. The root table is `usf::UsfMsg { 0: uint32 type; 1: [ubyte] data; }`.
  Messages larger than **1012 bytes** are split into FRAG messages.
* **Outer types (`usf::UsfMsgType`)**: 1 = REQ, 2 = RESP, 3 = FRAG, 4 = BATCH,
  5 = EVENT (a FlatBuffer event), 7 = transport test, 9 = COMPACT (raw binary
  sample batch).
* **Requests and responses.** `UsfMsg.data` holds a nested FlatBuffer
  "envelope" `{0: uint32 msg_id; 1: uint32 req_id; 2: uint32 server_handle (req) | UsfErr status (resp); 3: [ubyte] payload}`.
  The payload is another nested FlatBuffer that depends on `msg_id`. A
  response repeats the request's `msg_id` and `req_id`.
* **No handshake.** Nothing has to be sent before the first request. Clients
  start with `GetServer(uuid)` (msg_id 3) to the fixed server-manager handle
  **1**. This returns the handle of the sensor manager (or another server).
* **Samples** do not arrive as FlatBuffers. They come as `UsfMsg{type=9}`,
  whose `data` is a packed "compact sample batch": a 16-byte header, then
  `count` × (u64 timestamp|accuracy, N × float32).
* **Getting accel.** Send GetServer(sensor-manager UUID) → SensorList (5) →
  SensorInfo (4) for each handle → StartSampling (8) to the accel handle with
  `period_ns = 20 000 000`. Then read type-9 messages whose `sampling_id`
  matches. Finish with StopSampling (9).
* **Big caveat: the registry.** On Android, the HAL loads
  `/vendor/etc/sensors/registry/*.reg` into the AoC at every boot
  (RegistryLoadScript, id 24). It then sets `"/" loaded=1` with RegistrySet
  (id 22). The physical sensor drivers are almost certainly not created until
  that happens. Without it, expect an empty or virtual-only sensor list. This
  document describes those messages (§7). `usf.py` does not send them, because
  it is limited to read-only requests.
* **Display state.** The under-display TMD3719 (light and proximity) is timed
  to the panel. The HAL tells the AoC the panel's brightness (DBV), on/off/AOD
  state, refresh rate and operation rate with DisplayInfo (id 1001) to the
  display info server, at start and on every change. Before the registry
  load, it also writes the panel's name, serial and "original" flag to
  `/dev/display_info/0` (§10).

---

## 1a. Transport and framing

### Device handling

| What | Detail | Evidence |
|---|---|---|
| Devices | `/dev/acd-com.google.usf` (wake) and `/dev/acd-com.google.usf.non_wake_up`. `EfwTransportClient(char const*, char const*)` builds `"/dev/" + name`. | [V 0x59160], strings at 0x3a330 / 0x3a343 |
| open | `open(path, O_RDWR\|O_NONBLOCK)` (flags 0x802) on both. On EAGAIN/ENODEV it retries every 2 ms (`0x1e8480` ns), 6 attempts in total; `ENOENT` means "not ready". | [V 0x58b10..0x58cb0] |
| readiness | libusf treats "client ready" as "both device nodes exist". It checks with `stat()` in `Init`, and with an inotify/`MonitorFdThread` otherwise. **No message is exchanged.** | [V 0x58680, 0x597a0] |
| receive | `epoll` on both fds, then `read(fd, buf, 1024)`. **One read = one message**, which goes straight to `UsfTransport::ProcessMsg(buf, n, is_nwu)`. | [V 0x58e20, 0x59530..0x595a0] |
| send | `write(fd, desc->data, desc->size)` with the whole message and nothing prepended. A short write is an error. EAGAIN makes the transport queue the message and retry. The fd is the wake one unless `MsgDesc.non_wakeup` (byte at +16) is set. | [V 0x59400..0x59450] |
| max message | `EfwTransportClient::max_msg_size = 1012` (0x3f4). `UsfTransport::SendMsg` fragments anything larger. | [V 0x59394, 0xe1100..0xe1114] |

`MsgDesc` is built in `UsfTransport::SendMsg(UsfTxMsg*)` [V 0xe10a4..0xe10e0] from the
finished `FlatBufferBuilder`: `size = reserved - (cur - buf)`, `data = cur`. That is
exactly the finished FlatBuffer, so **there is no framing header of any kind.**

### Message = one FlatBuffer, root `usf::UsfMsg`

All data is little-endian, following standard FlatBuffers rules: the first u32
is the root table offset, tables start with an int32 soffset to their vtable,
vtables are `[u16 vtable_size, u16 table_size, u16 field_off...]`, and field N
sits at vtable byte offset 4 + 2N.

`UsfTransport::ProcessMsg` [V 0xe1b08] rejects anything with
`len < 5`, `root_off < 1`, `root_off > len-1`. It then runs the `UsfMsg`
verifier [V 0x8b060] with `max_depth = 64`, `max_tables = 1 000 000` and
`check_alignment = true` (constants at 0x3a310/0x3a270). That means:

* table offsets must be 4-aligned relative to the start of the buffer,
* vtable offsets must be 2-aligned, and vtable sizes even,
* scalars must be in bounds (this libflatbuffers version does not check scalar alignment),
* vectors must be in bounds, with a 4-aligned length field.

Nested FlatBuffers are verified again by their consumers, relative to the
start of the vector data. The AoC CPU is 32-bit ARM, so `usf.py` also places
nested buffers and u64 fields 8-aligned in absolute terms, which is safe
either way.

### No handshake

* `EfwTransportClient::Connect` [V 0x589f0] opens the fds, creates a pipe,
  starts `TransportThreadFunc` and returns. It sends nothing.
* `UsfTransportClient::SetIsConnected(true)` [V 0xe4180] only marks the
  local precondition "Transport ready" as met and dispatches a local
  connection event (`UsfTransport::SetIsConnected` [V 0xe3330]).
* `USF_API: Connecting to USF.` is `UsfApiImpl::Connect` [V 0x5bf40]. It
  creates the transport client and later runs `LookUpServers` [V 0x5dd30]. The
  first traffic on the wire is therefore an ordinary request:
  1. `IsRegistryLoaded()` [V 0x5e5a0]: `GetServer(registry UUID)`, then
     `RegistryGet("/")`, checking the property `loaded == "1"`. It retries
     until that is true.
  2. `GetServer(sensor-manager UUID)` → handle.
  3. `SensorList` → for each handle, `SensorInfo`.
* So **the host sends first, with any request** (normally
  `GetServer`, msg_id 3, to server handle 1). It expects a `UsfMsg{type=2}`
  carrying the same `req_id`. The AoC never pushes anything before it is asked.

---

## 1b. Outer table and envelope

### `usf::UsfMsg` (root of every channel message)

| slot | voffset | type | meaning | evidence |
|---|---|---|---|---|
| 0 | 4 | uint32 | `UsfMsgType` | [V 0x8b12c verifier; 0xe1bac dispatch] |
| 1 | 6 | [ubyte] | data: a nested FlatBuffer (types 1–5), or raw bytes (type 9) | [V 0x8b14c verifier; 0x7860c builder adds it at voffset 6] |

### `UsfMsgType` (dispatch in `UsfTransport::ProcessMsg` [V 0xe1bb4..0xe1cd8])

| value | name | handling | nested data |
|---|---|---|---|
| 1 | REQ | vtable[7] `ProcessReqMsg`. A client host can receive requests too; `usf.py` ignores them. | envelope |
| 2 | RESP | vtable[8] `UsfTransportClient::ProcessRespMsg` [V 0xe41c0] | envelope |
| 3 | FRAG | `ProcessFragMsg` [V 0xe27b0] | `UsfMsgFrag` |
| 4 | BATCH | `ProcessBatchMsg` [V 0xe2af0] | `UsfMsgBatch` |
| 5 | EVENT | vtable[9] `ProcessEventMsg` [V 0xb6340] → `DispatchTransportEventOnMsg(msg, 5)` | `UsfMsgEvent` |
| 7 | test | handler registered by `UsfTestTransport*` [V 0xd9bf4, 0xd9d48] | – |
| 9 | COMPACT | vtable[10] = [V 0x59c44] → `DispatchTransportEventOnMsg(msg, 9)` → `UsfTransportMsgEvent::Init(vec, raw=true)`: stored without verification | raw compact batch (§1c) |
| other < 10 | – | `UsfTransportMgr::msg_handler_table_[type]` (none registered besides 7). Otherwise the transport's default handler, or `"No handler for USF message with type %d."` | – |

### Envelope (name not exported): nested in `UsfMsg.data` for REQ and RESP

The same verifier [V 0x8a560] is used for requests (server side,
`UsfServerReq::Create` [V 0xaeb14]) and for responses (`UsfResp::SetData` [V 0x86da4]).

| slot | voffset | type | request | response | evidence |
|---|---|---|---|---|---|
| 0 | 4 | uint32 | msg_id (table below) | the same msg_id, repeated back | server dispatch reads it [V 0xadda8 etc.]; responses: `mov w1,#0x19` for GetTime [V 0xdc2bc], `#5` SensorList [V 0xa67bc], `#8` StartSampling [V 0x9b14c] |
| 1 | 6 | uint32 | req_id: atomic `UsfReqMgr::next_req_id_++`, skipping 0 | same req_id (copied by `UsfServer::SendResp`) | [V 0x85d20..0x85d88; 0xadb08..0xadba4]; matching [V 0xe4284] |
| 2 | 8 | uint32 | **server handle** of the target server | **status** (`UsfErr`, 0 = OK; omitted when 0) | routing [V 0xad49c..0xad4dc]; status [V 0x86dd8, 0xad960] |
| 3 | 10 | [ubyte] | payload: nested FlatBuffer (optional) | payload: nested FlatBuffer (optional; absent on error) | [V 0x635c0 `AddOffset(10,…)`; 0x87cf0 `UsfRespGetMsg` reads it] |

On the outer level the request is `UsfMsg{0: 1 (REQ), 1: envelope}`
[V 0x85e74 `PushU32(1)`/voffset 4 in `UsfReq::Finish`]. The response is
`UsfMsg{0: 2, 1: envelope}` [V 0x87144 `UsfServerResp::Finish`].

An unknown server handle gets a response with status 7 (NOT_FOUND) from
`SendErrorResp` [V 0xad4f4, 0xad694]. An unsupported msg_id gets status 8
(`"Received unsupported USF request type %d."` [V 0xaddfc]).

### Request/response msg_id values (envelope slot 0)

Each server's `ProcessReq` dispatches on the id [V at the cited address]:

| id | message | server (address of the `ProcessReq` switch) |
|---|---|---|
| 1 | Echo (any server) | `UsfServer::ProcessReq` 0xadd90 |
| 2 | StatGetRows | `UsfStatServer` 0xbc020 |
| 3 | GetServer | `UsfServerMgrServer` 0xae260 |
| 4 | SensorInfo | `UsfSensor` 0x99bb0 |
| 5 | SensorList | `UsfSensorMgrServer` 0xa6270 |
| 6 | SpiTransfer | `UsfSpiServer` 0xb92d0 |
| 7 | I2cTransfer | `UsfI2cServer` 0x73e30 |
| 8 | StartSampling | `UsfSensor` |
| 9 | StopSampling | `UsfSensor` |
| 10 | ReconfigSampling | `UsfSensor` |
| 11 | SyncTime | `UsfTimeServer` 0xdbe70 |
| 14 | FlushSamples | `UsfSensor` |
| 15 / 16 | Reg / UnregSampleChannel | `UsfSampleEventChannelServer` 0x8ec90 |
| 17 / 18 / 28 | Set / Clr / GetSampleTransform | `UsfSensor` |
| 19 | SelfTest | `UsfSensor` |
| 20 | UsbChargingCurrent | `UsfSensor` |
| 21 / 22 / 23 / 24 | RegistryGet / Set / Remove / LoadScript | `UsfRegistryServer` 0x80cf0 |
| 25 | GetTime | `UsfTimeServer` |
| 27 / 40 | RegistryProdScript / ProdScriptNode | `UsfRegistryServer` |
| 29 / 30 | StatList / StatReset | `UsfStatServer` |
| 31 | DmaWrite | `UsfDmaServer` 0x6d6c0 |
| 32 | Suez | `UsfSuezServer` 0xc4d10 |
| 33 / 34 | ShMemTransportInit / Deinit | `UsfShMemTransportMgrServer` 0xb7a90 |
| 35 | DebugGetBuffer | `UsfDebugServer` 0x68fe0 |
| 37 | ReportSensorActive (subscribe to a sensor's active/inactive changes; AoC side only) | sensor, sent by the HAL's `ProxMonitorService::SendReq` [HAL 0x2c24c] (§10.7) |
| 300 | ContextEvent | `UsfEventServer` 0x6f230 |
| 800–804 | Start/Stop/Data/Status/Repeated data injection | `UsfSensor` |
| 1001 | DisplayInfo (§10) | `UsfDispInfoServer` 0x6bca0 |
| 1007 | GetSensorListStatus | `UsfSensorMgrServer` |

The "UsfMsgType" values that the task asked for map as follows. The protocol
has no separate *response* ids and no separate "event" request.

| wanted | answer |
|---|---|
| sensor list req/resp | envelope id 5, both directions |
| sensor info req/resp | envelope id 4 |
| start sampling req/resp | envelope id 8 |
| stop sampling req | envelope id 9 |
| reconfig sampling | envelope id 10 |
| get time / sync time | envelope ids 25 / 11 |
| echo req/resp | envelope id 1 |
| event / sensor report | outer `UsfMsg.type` 9 (compact batch). Outer type 5 is used for FlatBuffer events. |
| handshake | none (see §1a). The first request is GetServer (id 3) to handle 1. |

### Server handles and UUIDs

* The server manager is fixed at **handle 1** [V 0xe4604: `PushU32(1)` → voffset 8
  in `UsfClientGetServer`]. Other handles are allocated from
  `UsfServerMgr::next_server_handle_` (initial value 2, at 0xf6440). Look them
  up with GetServer; never hardcode them. Sensors are servers too: every entry
  of the sensor list is a server handle (`UsfSensor` +72 = handle, [V 0xa647c], [V 0xad4d4]).
* UUIDs: 16 raw bytes, sent exactly as they are stored.

| server | UUID bytes | used by |
|---|---|---|
| sensor manager | `6b31cbf0744a14a1d4e8ae8a3e09fc4d` | `LookUpServers` [V 0x5ddc8 → 0x3a364] |
| registry | `d76a14cc264deea1c6406f8b461682c0` | `IsRegistryLoaded` [V 0x5e5f8 → 0x3a374] |
| time | `8bb81b18254c299f0a45deb4d8cbc541` | `UsfGetAndroidAndSensorCoreTime` [V 0x5a664 → 0x3a320] |
| stats | `cf79f8eaee434740535220b6b973febf` | `UsfStatUtils` [V 0xbeb4c → 0x3a84c] |
| DMA | `d1601cffab4decfba96f09b6c254bd06` | `UsfDmaMgr` (do not use) |
| shmem transport | `fa8e2a84234a79376879369cc51aa914` | shmem path (unavailable) |
| suez | `ae756f064c4f5a3bbcc39f8da106efc3` | `UsfSuezClient` |
| display info | `a88ec62410402aedcab9a5aad6a2eed2` | HAL `DisplayInfoService::Connect` [HAL 0x25190 → 0x161f8]; server copy [V 0x6c4a4 → 0x3a430] |

### FRAG (outer type 3): `usf::UsfMsgFrag`

| slot | type | meaning | evidence |
|---|---|---|---|
| 0 | uint32 | always 1 when sent (meaning unknown) | [V 0xe365c] |
| 1 | uint32 | total size of the reassembled message | [V 0xe2a44 → `UsfMsgReassembler::Create(size)`] |
| 2 | uint32 | byte offset of this fragment | [V 0xe29e4, 0xe3b2c] |
| 3 | [ubyte] | fragment bytes | [V 0xe3b10, 0xe3c68 memcpy] |

Offset 0 starts a new reassembly buffer. Other fragments must arrive in
order (`"Expected fragment offset %jd, but got …"`). Once the collected bytes
equal the total, the buffer is a complete `UsfMsg` and is passed through
`ProcessMsg` again [V 0xe2aac]. The sender uses chunks of
`max_msg_size - frag_msg_overhead_` [V 0xe26e0..0xe26f8].

### BATCH (outer type 4): `UsfMsgBatch { 0: [UsfMsgBatchEntry] }`, `UsfMsgBatchEntry { 0: [ubyte] msg }`

Every entry holds a complete `UsfMsg`, which is processed recursively
[V 0xe2d20..0xe2d44 verify, 0xe2e28..0xe2df4 dispatch].

### EVENT (outer type 5): `usf::UsfMsgEvent { 0: uint32 event_type; 1: [ubyte] nested payload }`

Verified by the same 2-field verifier [V 0xb63e0]. `event_type == 6` means
"shared-memory FIFO head changed" [V 0xb6420]. `UsfApiImpl::HandleUsfEvent`
[V 0x5d958] passes `(event_type, payload)` to the client callback. The HAL
handles types 5 and 12 [HAL 0x37140]; they are not needed for sampling.

---

## 1c. Individual messages

Slots are FlatBuffers field indices. The voffset is 4 + 2·slot. "Payload"
means the nested FlatBuffer in envelope slot 3.

### GetServer (id 3): request to handle 1

* `UsfMsgGetServerReq { 0: [ubyte] uuid }`. At least 16 bytes are needed, and only the first 16 are used.
  [V 0xe4498..0xe44fc builder; 0xae42c length check `cmp w3,#0x10`; 0xae664 copies 16 bytes]
* `UsfMsgGetServerResp { 0: uint32 server_handle }` [V 0xe47fc; server 0xae6a8]. An unknown UUID → status 7.
* The client waits 5 s (`0x12a05f200` ns) [V 0xe46d8].

### Echo (id 1): any handle, e.g. 1

* `UsfMsgEchoReq { 0: [ubyte] data }`, `UsfMsgEchoResp { 0: [ubyte] data }`. The server copies the data back
  [V 0xadf14..0xadfd4; verifiers 0xaf4c0 / 0x873b0].

### SensorList (id 5): request to the sensor-manager handle, **no payload**

* The request has no payload [V 0x5dec0..0x5e0d8 (no `SetPayload`)].
* `UsfMsgSensorListResp { 0: [uint32] sensor_handles }`
  [V verifier 0x89370 (element size 4, no sub-tables); server 0xa647c pushes `sensor+72`; client 0x5e26c].

### SensorInfo (id 4): request to a **sensor handle**, **no payload**

`UsfMsgSensorInfoResp`. Types come from the verifier [V 0x88f80]. Sources are
the server builder `UsfSensor::ProcessSensorInfoReq` [V 0x99cd0] and the HAL
consumer `SensorHal::GetUsfSensorInfo` [HAL 0x2f69c..0x2f9cc].

| slot | type | meaning | confidence |
|---|---|---|---|
| 0 | [byte] | name. This is a C string: the vector's **last byte must be NUL** (`UsfFlatBuffersGetCString` [V 0x72974]) | V (HAL logs "No valid sensor name") |
| 1 | uint32 | `UsfSensorType` | V (`sensor+220`, same field as the compact header type) |
| 2 | float | max range | I (becomes hidl `SensorInfo.maxRange` via `InitHalSensorInfo` [HAL 0x48bdc]) |
| 3 | uint64 | min sampling period, ns (clamped by `GetMinMinDelayNs`) | V for the source, I for the name |
| 4 | uint32 | FIFO reserved event count | I (hidl fifoReserved) |
| 5 | uint32 | FIFO max event count | I |
| 6 | uint64 | max sampling period, ns (`sensor+240`) | I |
| 7 | [byte] | vendor | V (HAL: "No valid sensor vendor") |
| 8 | bool | flag (`sensor+282`, also gates `src` in the reporter) | I: "supports SRC"? |
| 9 | float | resolution | I (hidl resolution) |
| 10 | [byte] | registry node path (`GetNodePath(sensor+680)`), e.g. `/dev/lsm6dsv/0/accel` | V |
| 11 | uint32 | index among sensors of the same type (`sensor+296`) | V: stored at `UsfApiSensorInfo`+8 [V 0x5e500], and `UsfApiImpl::GetSensorInfo(type, index)` matches on (type, index) [V 0x5d4f0..0x5d504] |
| 12 | uint64 | raw/unclamped min period, ns | I |
| 13 | bool | flag (`sensor+312`) | unknown |

### StartSampling (id 8): request to a **sensor handle**

`UsfMsgStartSamplingReq`. Types come from the verifier [V 0xb1da0]. Values and
defaults come from the client builder `UsfStartSamplingReq::Finish`
[V 0xa7700] fed by `UsfApiImpl::StartSampling` [V 0x5c7e0], plus the HAL's
`Sensor::StartSampling` [HAL 0x44e90]. Meanings come from
`UsfSampleReporter::Init` [V 0x95380], whose values land in the stat
table `kSamplingConfigStatColList` (labels at 0xf0ed0), and from
`SendCompactSampleBatchMsg` [V 0x8ced4].

| slot | type | meaning | value used by UsfApiImpl / HAL | confidence |
|---|---|---|---|---|
| 0 | uint32 | `rpt_mode` (1 continuous, 2 on-change, 3 one-shot, 4 "count") | caller's mode | V (stat "rpt_mode"; names [HAL 0x44190]) |
| 1 | uint64 | **sampling period, ns** | caller's period | V (stat "period_ns") |
| 2 | uint64 | **max report latency, ns** (batching) | caller's latency | V (stat "max_latency_ns") |
| 3 | bool | `src` (sample-rate conversion; only if the sensor supports it) | 0 / HAL per sensor | V |
| 4 | bool | `passive` | 0 | V (stat "passive") |
| 5 | bool | `non_wake_up`: deliver on the `.non_wake_up` channel | 0 / HAL `!wakeup` | V (stat; sets `UsfTxMsg` non-wakeup flag [V 0x8d044]) |
| 6 | uint32 | sample channel handle: 0 = send batches as messages on the requesting transport; otherwise a channel registered with id 15 | 0 | V (`GetChannel` [V 0x8ca0c]) |
| 7 | uint32 | unknown | 0 | – |
| 8 | uint32 | unknown | 0 | – |
| 9 | uint32 | `transform_lvl` (default 3 when absent) | 3 / HAL per sensor | V (default [V 0x9550c]) |
| 10 | uint64 | `client_id`. Its low 32 bits come back in every compact batch header | caller's id | V [V 0x8d068, 0x957c0] |
| 11 | bool | timestamps in Android time (`UsfTimeMgr::GetAndroidTimeNs`) instead of sensor-core time | 0 / **HAL: 1** | V [V 0x8d080, 0x8d148] |
| 12 | bool | `lp_memory` | 0 | V (stat) |
| 13 | bool | `include_bias` (appends bias values to each sample) | 0 / HAL per sensor | V (stat) |
| 14 | bool | skip the min-delay clamp | 0 | V (code path [V 0x9560c]), I for the name |
| 15 | uint32 | unknown | 1 (both callers) | – |
| 16 | uint32 | sampling id to **reconfigure** (0 = new subscription) | 0 | V [V 0x9ad60 → `FindReporter` / `Reconfig`] |

The HAL sends the StartSampling request for a non-wake-up sensor on the `.non_wake_up` channel: it copies `!wakeup` into `UsfReq`+9, which becomes the TxMsg non-wakeup flag [HAL 0x451a0; V 0x85ef8]. `UsfApiImpl` and `usf.py` always use the wake channel.

`UsfMsgStartSamplingResp { 0: uint32 sampling_id }` [V verifier 0x89a50; server
0x9b030 pushes `reporter+240`, which is allocated from `next_sampling_id_`
[V 0x95958]]. The same sampling id appears in every compact batch header.

### StopSampling (id 9): request to the same **sensor handle**

`UsfMsgStopSamplingReq { 0: uint32 sampling_id }` [V `UsfApiImpl::StopSampling`
0x5d164 (`mov w1,#9`), 0x5d23c (loads the id stored from the start response),
0x5d2b8 (sensor handle to voffset 8); verifier 0xb2440]. The response has no
payload.

### ReconfigSampling (id 10)

`UsfMsgReconfigSamplingReq`, verifier [V 0xb0030]: 0 u32, 1 u64, 2 u64, 3 u8/?, 4 u8.
Not needed; StartSampling with slot 16 does the same job.

### GetTime (id 25): request to the time server, no payload

`UsfMsgGetTimeResp { 0: uint64 time_ns }`. This is the server's
`UsfGetCurrentTimeNs()` [V server 0xdc1d8..0xdc224; client 0x5a938].
On the AoC this is the sensor-core clock in ns (**[I]**).

### SyncTime (id 11): *not used by usf.py*

`UsfMsgSyncTimeReq { 0: uint64 android_ns, 1: uint64 sensor_core_ns }`
[V builder 0x6516c/0x65228 from `UsfGetAndroidAndSensorCoreTime(&android,&core)`;
server 0xdbfc0 → `UsfTimeMgr::SyncAndroidTime`]. The field order follows the
argument order, so treat it as **[I]**. This sets the AoC's Android-time
offset, which slot 11 of StartSampling needs.

### RegistryGet (id 21, read-only): registry server

* `UsfMsgRegistryGetReq { 0: [byte] node_path }`, a NUL-terminated C-string vector [V verifier 0xb05a0].
* `UsfMsgRegistryGetResp { 0: [Prop], 1: [?] }`, where `Prop { 0: [byte] name, 1: [byte] value }` [V 0x88590; parse 0x5ea54..0x5ec54].
  `IsRegistryLoaded` checks that `"/"` has `loaded == "1"`.

### Compact sample batch: `UsfMsg{type=9}.data` (raw, not a FlatBuffer)

The reader is `CreateSensorSampleReport` [V 0xa7300] plus the accessors of
`UsfCompactSensorSampleReport<…>` [V 0xa73a0..0xa76f4]. The writer is
`UsfMsgSampleEventChannel::SendCompactSampleBatchMsg` [V 0x8ced4]. The
layout below is confirmed from both sides.

Format 1 (`UsfCompactSampleBatchHeader`):

| offset | size | field |
|---|---|---|
| 0 | u32 | format = 1 |
| 4 | u32 | client_id (StartSampling slot 10, low 32 bits) |
| 8 | u32 | sampling_id (from StartSamplingResp) |
| 12 | u32 | bits 0–15 `UsfSensorType`, bits 16–25 sample count, bits 26–31 values per sample N |
| 16 + i·(8+4N) | u64 | bits 0–59 timestamp (ns), bits 60–63 `UsfSampleAccuracy` |
| 24 + i·(8+4N) | N × f32 | values |

Format 2 (`UsfCompactLargeSampleBatchHeader`, used when N > 63): as format 1,
but the type/count/N field is 40 bits wide at offset 12, with N in bits
26–35. The samples start at offset **17**, unaligned [V 0x8d3f8..0x8d434, 0xa7560..0xa76f4].

Accuracy [HAL 0x48c80]: 0 = default (becomes Android HIGH), 1 = unreliable,
2 = low, 3 = medium, 4 = high.

Timestamps: with slot 11 = 0 (what `UsfApiImpl` and `usf.py` send), the
value is the sensor-core (AoC) clock in ns. With 1, the AoC maps it to
Android boottime using the SyncTime offset. The AoC clock is a 24.576 MHz
tick counter: `aoc_ticks_to_nanoseconds(t) = (t·2666667 + 0x8000) >> 16`
[V aoc 0x11b0]. libusf pairs it with kernel boottime by reading
`/sys/devices/platform/19000000.aoc/aoc_clock_and_kernel_boottime`
[V 0x5a234, string 0x39b9c]. It can also use GetTime and take the
midpoint of the host clock around the request [V 0x5a90c].

Values: floats copied straight into Android events (`Sensor::GetSampleData(Vec3)`),
so they are presumably in Android units: m/s² for accel, rad/s for gyro, µT
for mag, hPa, lux, cm (**[I]**).

---

## 1d. Sensor types and rate units

`usf::UsfSensorType` numbers come from the HAL's `Sensor::GetDescription`
table [HAL 0x44694..0x44de8]. The type→name pairs were read from the static
initialiser; the ones marked * are the ones this project needs:

| id | name | | id | name |
|---|---|---|---|---|
| **1*** | accelerometer | | 20 | game rotation vector |
| **2*** | gyroscope | | 21 | geomagnetic rotation vector |
| **3*** | proximity | | 22 | gravity |
| **4*** | pressure | | 23 | linear acceleration |
| 5 | pressure temperature | | 24 | orientation |
| **6*** | magnetometer | | 27 | rotation vector |
| 7 | magnetometer temperature | | 28 | significant motion |
| 8 | IMU temperature | | 29 / 30 | step detector / counter |
| **9*** | ambient light | | 31 / 32 / 33 | tilt / motion / stationary detect |
| 12 / 13 / 14 | spectral / flicker / binned brightness | | 45 | hall effect |
| 16 / 17 | device orientation / pickup | | 53 | hinge angle |

The full list (1–82) is in `usf.py:SENSOR_TYPE_NAMES`. A few names in the
list's tail (38, 44, 55) were paired heuristically, so treat those as **[I]**.

Rates: StartSampling slot 1 is the **sampling period in nanoseconds**
(50 Hz = 20 000 000). Slot 2 is the **max report latency in nanoseconds**
(0 = deliver each sample as soon as possible). `UsfApiImpl::StartSampling(type, period_ns, latency_ns, mode, index, client_id, non_wakeup)`
passes them straight through [V 0x5c9c8/0x5c9fc → object +456/+464 → voffsets 6/8].
For continuous sensors the AoC clamps the period to `GetMinMinDelayNs` unless slot 14 is set [V 0x95610].

---

## 1e. Minimal client sequence (what `usf.py` does)

1. `open("/dev/acd-com.google.usf", O_RDWR|O_NONBLOCK)`. Optionally open the `.non_wake_up`
   twin for reading.
2. **GetServer**: msg 3 → handle 1, payload `{uuid: 6b31cbf0…fc4d}`. The response
   payload `{0: handle}` is the sensor-manager handle `M`.
3. **Echo** (optional sanity check): msg 1 → handle 1, payload `{data}`. The response returns the same bytes.
4. **SensorList**: msg 5 → handle `M`, no payload. The response is a `[uint32]` list of sensor handles.
5. **SensorInfo** per handle: msg 4 → sensor handle. Pick the one with `type == 1`
   (and the wanted `index`, slot 11).
6. **StartSampling**: msg 8 → accel handle, with `period_ns = 20 000 000`, `latency = 0`,
   `rpt_mode = 1`, `transform_lvl = 3`, `client_id = X`, `slot15 = 1`, everything else 0.
   The response `{0: sampling_id S}` gives the subscription id.
7. Read messages. For each `UsfMsg.type == 9` whose header has `sampling_id == S`,
   decode the samples. FRAG and BATCH wrappers may appear and must be unwrapped.
8. **StopSampling**: msg 9 → accel handle, payload `{0: S}`. Wait for the status 0 response.

Request ids start at 1 and increment. Match each response by `req_id`. Handles
are dynamic, so always resolve them at run time.

### Example bytes (generated by `usf.py`; handles are placeholders)

GetServer(sensor manager) → handle 1, req_id 1 (112 bytes):
```
  0000  10 00 00 00 00 00 00 00 08 00 0c 00 08 00 04 00   root→0x10 | pad | UsfMsg vtable: 8B, tbl 12B, slot0@+8, slot1@+4
  0010  08 00 00 00 08 00 00 00 01 00 00 00 50 00 00 00   UsfMsg: soffset 8 | data→0x1c | type=1 REQ | data len 0x50
  0020  10 00 00 00 0c 00 14 00 10 00 0c 00 08 00 04 00   envelope root→0x10 | vtable: 12B, tbl 20B, slots@+16,+12,+8,+4
  0030  0c 00 00 00 10 00 00 00 01 00 00 00 01 00 00 00   envelope: soffset 12 | payload→ | handle=1 | req_id=1
  0040  03 00 00 00 28 00 00 00 0c 00 00 00 00 00 06 00   msg_id=3 GetServer | payload len 0x28 | GetServerReq root→0xc ...
  0050  08 00 04 00 06 00 00 00 04 00 00 00 10 00 00 00   ... vtable, table, uuid vector len 16
  0060  6b 31 cb f0 74 4a 14 a1 d4 e8 ae 8a 3e 09 fc 4d   sensor-manager UUID
```

SensorList → sensor-manager handle 2, req_id 3 (64 bytes):
```
  0000  10 00 00 00 00 00 00 00 08 00 0c 00 08 00 04 00
  0010  08 00 00 00 08 00 00 00 01 00 00 00 20 00 00 00
  0020  10 00 00 00 00 00 0a 00 10 00 0c 00 08 00 04 00
  0030  0a 00 00 00 02 00 00 00 03 00 00 00 05 00 00 00   envelope: handle=2 req_id=3 msg_id=5 (no payload)
```

SensorInfo → sensor handle 10, req_id 4 (64 bytes):
```
  0000  10 00 00 00 00 00 00 00 08 00 0c 00 08 00 04 00
  0010  08 00 00 00 08 00 00 00 01 00 00 00 20 00 00 00
  0020  10 00 00 00 00 00 0a 00 10 00 0c 00 08 00 04 00
  0030  0a 00 00 00 0a 00 00 00 04 00 00 00 04 00 00 00
```

StartSampling(handle 10, 20 ms, latency 0, client 0x4C494E58), req_id 5 (184 bytes):
```
  0000  10 00 00 00 00 00 00 00 08 00 0c 00 08 00 04 00
  0010  08 00 00 00 08 00 00 00 01 00 00 00 98 00 00 00
  0020  10 00 00 00 0c 00 14 00 10 00 0c 00 08 00 04 00
  0030  0c 00 00 00 10 00 00 00 0a 00 00 00 05 00 00 00   handle=10 req_id=5
  0040  08 00 00 00 70 00 00 00 30 00 00 00 00 00 00 00   msg_id=8 | payload len 0x70 | StartSamplingReq root→0x30
  0050  00 00 26 00 40 00 24 00 38 00 30 00 0b 00 0a 00   vtable: 38B (17 slots), table 64B, slot offsets...
  0060  09 00 20 00 1c 00 18 00 14 00 28 00 08 00 07 00
  0070  06 00 05 00 10 00 0c 00 26 00 00 00 00 00 00 00   table: soffset 0x26 | slot16=0 ...
  0080  00 00 00 00 00 00 00 00 01 00 00 00 03 00 00 00   slot15=1, slot9=3 (transform level)
  0090  00 00 00 00 00 00 00 00 00 00 00 00 01 00 00 00   slot0=1 (continuous)
  00a0  58 4e 49 4c 00 00 00 00 00 00 00 00 00 00 00 00   slot10 client_id (u64) | slot2 latency=0 (u64)
  00b0  00 2d 31 01 00 00 00 00                           slot1 period = 0x01312d00 = 20 000 000 ns
```

StopSampling(handle 10, sampling id 100), req_id 6 (96 bytes):
```
  0000  10 00 00 00 00 00 00 00 08 00 0c 00 08 00 04 00
  0010  08 00 00 00 08 00 00 00 01 00 00 00 40 00 00 00
  0020  10 00 00 00 0c 00 14 00 10 00 0c 00 08 00 04 00
  0030  0c 00 00 00 10 00 00 00 0a 00 00 00 06 00 00 00
  0040  09 00 00 00 14 00 00 00 0c 00 00 00 00 00 06 00   msg_id=9 | payload 20B
  0050  08 00 04 00 06 00 00 00 64 00 00 00 00 00 00 00   sampling_id = 100
```

The expected StartSampling response (built by the simulator the same way the server builds it):
```
  0000  10 00 00 00 00 00 00 00 08 00 0c 00 08 00 04 00
  0010  08 00 00 00 08 00 00 00 02 00 00 00 40 00 00 00   type=2 RESP
  0020  10 00 00 00 0c 00 14 00 10 00 0c 00 08 00 04 00
  0030  0c 00 00 00 10 00 00 00 00 00 00 00 05 00 00 00   status=0 req_id=5
  0040  08 00 00 00 14 00 00 00 0c 00 00 00 00 00 06 00   msg_id=8
  0050  08 00 04 00 06 00 00 00 64 00 00 00 00 00 00 00   sampling_id=100
```
(The real AoC orders the fields differently, but only the vtable is different.)

A compact accel batch with 2 samples (type 9):
```
  0000  10 00 00 00 00 00 00 00 08 00 0c 00 08 00 04 00
  0010  08 00 00 00 08 00 00 00 09 00 00 00 38 00 00 00   type=9, 56 data bytes
  0020  01 00 00 00 58 4e 49 4c 64 00 00 00 01 00 02 0c   fmt 1 | client | sampling 100 | type 1, count 2, N=3
  0030  08 1a 99 be 1c 00 00 40 0a d7 23 3c 0a d7 a3 bc   ts 123456789000, acc 4 | x y
  0040  c3 f5 1c 41 08 47 ca bf 1c 00 00 40 0a d7 a3 3c   z | ts2 ...
  0050  0a d7 23 bc cd cc 1c 41
```

---

## Using `usf.py`

```
python3 usf.py --self-test            # 107 offline checks (encoders <-> decoders, fragments, batch, flow, DisplayInfo)
python3 usf.py --dry-run              # print hex of every request; a built-in fake AoC answers
python3 usf.py [/dev/acd-com.google.usf] [--rate 50] [--duration 3] [--type 1] [--index 0] [-v]
python3 usf.py --check-registry       # also does the read-only RegistryGet("/") to see whether loaded=1
python3 usf.py --get-time             # also does the read-only GetTime (sensor-core clock vs host CLOCK_BOOTTIME)
```

`encode_request()` refuses any msg_id outside {3, 1, 5, 4, 8, 9, 21, 25, 22,
24, 1001}. The command line sends 21 and 25 only with the opt-in flags and
22/24 only with `--load-registry`; 1001 (DisplayInfo) is for callers such as
`pixel-sensor-proxy`, through `UsfClient.display_info()`. StopSampling is
always sent, even if the sampling loop is interrupted.

## 7. The registry (why the sensor list may be empty) — not implemented

Android's `SensorHal::LoadRegistry` [HAL 0x315e0] runs once per AoC boot. It
is skipped only when `IsRegistryLoaded` is already true.

1. Load every `*.reg` file from `/vendor/etc/sensors/registry`, `…/registry/append`,
   `/mnt/vendor/persist/sensors/registry` and `/data/vendor/sensors/registry`
   (`kRegistryLoadPathList` [HAL 0x51470]). `UsfRegistryStoreFile::LoadFromFilePath` [V 0x850d0]
   sends each file as **RegistryLoadScript** (id 24) to the registry server:
   `UsfMsgRegistryLoadScriptReq { 0: [byte] script text, 1: uint32 device CDT info, 2: uint32 device MLB info }`
   [V builder 0x838b0, verifier 0xb0730]. Scripts are about 12 KB each, so they go out
   as FRAG messages. The CDT filter lines (`?+0x30000:0xFFFF0000` …) are
   evaluated against the CDT value sent in slot 1. Libusf reads it from
   `/sys/firmware/devicetree/base/chosen/plat…` and `…/config/pcbcfg`.
2. Send **RegistrySet** (id 22): `UsfMsgRegistrySetReq { 0: [byte] node path "/", 1: [Prop{name,value}] }`
   with `loaded = "1"` [HAL 0x3187c, verifier 0xb0e50].

The AoC's startup phases ("Registry Loaded" → "Physical Devices Ready" →
"Virtual Devices Ready" → "USF Ready", strings in libusf, `UsfStartupMgr`)
suggest that the physical drivers (`/dev/lsm6dsv/0/accel` and so on) only
appear after step 2 (**[I]**). `usf.py --check-registry` performs the
read-only RegistryGet(`"/"`) so you can tell whether this is the case.
Doing steps 1–2 means registry writes, which this tool deliberately does not do.

---

## 8. Status codes (`UsfErr`, envelope slot 2 of responses)

The binaries contain no name table. These are the codes returned at the cited
sites; the meanings are **[I]**: 0 OK · 2 generic failure · 3 size/overflow ·
5 invalid/malformed (e.g. UUID shorter than 16) · 6 no memory · 7 not found
(unknown server handle or UUID) · 8 unsupported request · 11 not connected ·
12 not ready · 14 I/O · 17 busy (EAGAIN) · 19 already started · 20 out of range.

---

## 9. Unresolved and caveats

* Nothing here has been run against the phone. The AoC firmware may differ
  from this AP-side libusf build, but the two ship together in the same
  vendor image.
* StartSampling slots 7, 8 and 15, SensorInfo slots 8 and 13, and FRAG slot 0
  have no known meaning. `usf.py` sends the same values as `UsfApiImpl`.
* SensorInfo scalar meanings (range, resolution, FIFO, max period) come from
  how the HAL consumes them, not from names.
* A few sensor-type names outside the set needed here were paired
  heuristically.
* The EVENT (type 5) `event_type` enum is only partly known (6 = FIFO head
  changed). Samples do not use it.
* The AoC may send requests of its own to the host (type 1). `usf.py` logs
  and ignores them; libusf would route them to local servers.
* The registry prerequisite (§7) is the most likely reason for an empty
  sensor list on a non-Android host.
* DisplayInfo (§10) was recovered from the HAL (sender) and libusf's copy of
  the server. How the AoC's TMD3719 driver uses each value is known only from
  its log strings in `aoc.bin`; that code was not disassembled. It is untested
  whether reporting the display state brings back a stalled TMD3719.

---

## 10. Display info (id 1001): the panel state the TMD3719 needs

Recovered from `sensors.usf.so` (`usf::DisplayInfoService`,
`SensorHal::AddPanelInfosToRegistry`, `usf::ProxMonitorService`) and from the
server copy in `libusf.so` (`usf::UsfDispInfoServer`). `DisplayInfoService` is
only built into the HAL; `usf.py` now has the encoder
(`encode_display_info_req`, `UsfClient.display_info`).

### 10.1 Why the TMD3719 needs it

The stock registry times the under-display TMD3719 to the panel:
`/dev/tmd3719/0` has `sync_delay_ns` and `te2_alignment=1`; `.../als` has
`EM_cycle_count_60Hz`, `itime_table_brightness`/`itime_table` (integration time
by DBV) and the histogram/leakage settings; `.../prox` has
`burst_delay_30hz/60hz/120hz/aod` and `min_fps=30`. The AoC can only choose
among those with the panel state that the HAL reports. The AoC firmware
(`/vendor/firmware/aoc.bin`) contains the same server and a TMD3719 driver
that consumes these reports. That is known from its log strings only
(**[I]**, not disassembled):

| `aoc.bin` string | what it says about the inputs |
|---|---|
| `USF: brightness:%u refresh_rate:%u te2_rate:%u status:%u display_id:%u` | same server code as libusf's copy (§10.2) |
| `USF: TMD3719: Failed to register display info event listener, err = %d` | the TMD3719 driver listens for DisplayInfo |
| `USF: TMD3719: dbv:%d leakage:%f refresh_rate:%d status:%d op_hz:%d [%d]` | logged per report on stock (e.g. `dbv:372 leakage:1.432487 refresh_rate:60 status:1 op_hz:120`) |
| `USF: TMD3719: Apply NS mode ALS delay: %d` / `Apply HS mode ALS delay: %d` | `op_hz` selects the ALS delay (60 Hz NS or 120 Hz HS operation) |
| `USF: TMD3719: No burst delay setting that matches the current FPS(%d)` | the refresh rate selects the prox `burst_delay_*hz` |
| `USF: TMD3719: Failed to update VSYNC frequency. (%d)`, `HealthCheck-VSYNC_FREQ misconfigured (%x != %x)` | the chip's VSYNC frequency register is set from the refresh rate |
| `USF: TMD3719: [ALS]sync_delay_adjustment: %lld`, `Sync delay shouldn't be less than 0 after adjustment.` | the sync delay is adjusted per refresh rate/EM cycle; stock logs `1217995`, Linux logs the error |
| `USF: TMD3719: Watchdog[l:%d p:%d s:%d]: status:%x enable:%x/%x!` | the `status` in the watchdog line is the chip's STATUS register (hex), not the display state |
| `USF: TMD3719: Panel name is %s`, `Panel serial is %s`, `panel_is_original is %d`, `Panel max refresh rate is %d`, `No node found for '%s' panel name.` | reads `/dev/display_info/0` (§10.6) and the `/dev/tmd3719/0/<panel_name>` subtree |
| `USF: TMD3719: Panel is original but serial(cur:%s, fac:%s) don't match.`, `Panel is not original but serial(%s) match.`, `Applying factory-calibrated parameters for ALS.` / `for PROX.` | the factory calibration (from persist) is checked against `panel_serial` and `panel_is_original` |

Until now the Linux side sent none of it, so the AoC ran without a refresh
rate, DBV or operation rate (**[I]**: presumably all 0).

### 10.2 Transport

| What | Detail | Evidence |
|---|---|---|
| server | "Display Info Server", UUID `a88ec62410402aedcab9a5aad6a2eed2`. Look it up with GetServer (id 3) on handle 1. | HAL `Connect` [HAL 0x25190 → 0x161f8]; `UsfDispInfoServerMgr::Init` copies the same UUID into the server and names it [V 0x6c4a4 → 0x3a430, 0x6c4e0] |
| request | `UsfMsg{type 1 REQ}`, envelope `{0: 1001, 1: req_id, 2: display info server handle, 3: UsfMsgDisplayInfoEvent}` | msg id [HAL 0x22a64 `mov w1,#0x3e9` → voffset 4]; handle = `DisplayInfoService`+0 [HAL 0x21ff0 → voffset 8]; payload [HAL 0x229ec..0x22a1c, then `UsfReq::Finish`] |
| dispatch | `ProcessReq` sends id 1001 to `ProcessDisplayInfo` and everything else to the base server (Echo, else status 8) | [V 0x6bca0] |
| response | RESP, status 0, **no payload**. A missing or malformed payload returns status 5 (`SendErrorResp`). | [V 0x6c08c `SendResp` with an empty `UsfServerResp`; 0x6be4c; 0xad764..0xad778] |
| wait | `SendSyncRequest` with a 5 s timeout; the response payload is ignored | [HAL 0x220bc..0x220d4: x2 = NULL, x3 = 0x12a05f200] |
| effect | the server logs the values, builds a `UsfDispEvent` and dispatches it to the display-event listeners | [V 0x6c1a8, 0x6c34c..0x6c3c4] |
| channel | wake channel: the non-wakeup byte `UsfReq`+9 stays 0 | [HAL 0x21eac zeroes it]; meaning of +9 from §1c |

### 10.3 Payload: `usf::UsfMsgDisplayInfoEvent`

The table name comes from `UsfFbb<UsfMsgDisplayInfoEventBuilder>` [HAL
0x25cd0]. Types are from the server's verifier [V 0x6bf70..0x6c088]: slots 0,
2, 4, 5, 6 and 7 are 4 bytes, slots 1 and 3 are 1 byte. Names are from the
server log line [V 0x6c1a8, 0x6c220], the HAL dump labels
(`DisplayInfoContent::DumpInfo` [HAL 0x20258..0x20328]) and the AoC log line.

| slot | voffset | type | name | meaning, units, values | HAL source | evidence |
|---|---|---|---|---|---|---|
| 0 | 4 | uint32 | brightness | panel DBV: the brightness register value (DCS 0x51). The stock AoC log shows values from 31 to 372. | `strtol` of `/sys/class/backlight/panel0-backlight/brightness` [HAL 0x216e8], read again at each histogram tick [HAL 0x219e4] | [V 0x6bfb4, 0x6c0f8 `brightness:%u`]; [HAL 0x224ac] |
| 1 | 6 | uint8 | status | 0 off, 1 on, 2 LP (AOD/doze), 3 HBM, 4 LHBM (local HBM on) | the backlight `state` text (§10.4); 4 whenever `local_hbm_mode` ≥ 1 | [V 0x6bfd0 (1 byte), 0x6c178 `status:%u`]; [HAL 0x226d0..0x22714, 0x22950] |
| 2 | 8 | uint32 | refresh_rate | Hz of the current mode; 0 when off; 30 in LP | the number after `@` in `state` (60 if there is none) | [V 0x6bfec, 0x6c128]; [HAL 0x2255c, 0x22df8] |
| 3 | 10 | uint8 | display_id | panel index: 0 primary, 1 secondary. It is `SendInfoReq`'s argument. | – | [V 0x6c008 (1 byte), 0x6c194]; [HAL 0x220b4 → 0x2277c] |
| 4 | 12 | float | leakage | the panel's own light reaching the sensor (**[I]**: lux), from `LeakageCalculator` (§10.5); 0 while the histogram is inactive | `LeakageCalculator::Calculate(luma, dbv)` [HAL 0x219fc] | [V 0x6c310, 0x6c220 `leakage:%f`]; [HAL 0x22840] |
| 5 | 14 | float | luma | mean bin index of the display histogram over the sensor's window, Σ i·bin[i] / Σ bin[i] (**[I]**: 0–255); `histo_fallback_luma` (128.0) when the histogram counts no pixels; 0 while inactive | IDisplay histogram API [HAL 0x24eec..0x24f1c, 0x24f4c] | [V 0x6c324, `luma:%f`]; [HAL 0x228c8] |
| 6 | 16 | uint32 | op_hz | panel operation rate in Hz (120 HS, 60 NS) | `strtol` of `/sys/class/drm/card0/device/primary-panel/op_hz` [HAL 0x21844]; **120** when that node is missing [HAL 0x215a4] | [V 0x6c05c (4 bytes); libusf's copy keeps one byte, 0x6c338/0x6c3ac]; [HAL 0x2266c]; AoC log `op_hz:120` |
| 7 | 18 | uint32 | te2_rate | TE2 rate in Hz | `.../primary-panel/te2_rate_hz`, read only when the ALS node has `is_te2_node_enabled` ≥ 1 [HAL 0x20c10, 0x2147c, 0x21754]. Cheetah's registry does not set it, so stock never sends this field. | [V 0x6c078, 0x6c164 `te2_rate:%u`]; [HAL 0x225ec] |

The HAL's builder has `force_defaults` off, so it **leaves out fields that are
0** [HAL 0x224b0/0x224ec, 0x21e70]. The server reads a missing field as 0
[V 0x6c100..0x6c19c]. `encode_display_info_req` does the same.

### 10.4 How the HAL gets the state (`PopulateStateAndRateInfoFromStringLocked`)

`DisplayInfoContent`, one per panel at `DisplayInfoService`+0x88+0x30·p [HAL
0x21f64..0x21fe4]: +0 time (s), +8 time (ms, `CLOCK_REALTIME_COARSE`; for the
dump only), +0x10 DBV, +0x14 LHBM, +0x18 RR, +0x1c TE2, +0x20 status, +0x24
leakage, +0x28 luma, +0x2c op_hz.

The text of `/sys/class/backlight/panel<p>-backlight/state` is parsed like this
[HAL 0x22b30]. The first test that matches anywhere in the text wins:

| text contains | status | refresh_rate |
|---|---|---|
| `HBM` | 3 | `strtol` after the first `@`, 60 if there is no `@` |
| `On` | 1 | the same |
| `Off` | 0 | 0 |
| `LP` | 2 | 30 |
| none | unchanged | unchanged |

The stock panel driver's text is presumably `On: 1440x3120@120`,
`HBM: 1440x3120@60`, `LP` or `Off` (**[I]**: inferred from the parser). The
matching AoC log lines show `status:1 refresh_rate:60/120` when on and
`status:2 refresh_rate:30` in AOD. `local_hbm_mode` (0/1) is kept separately.
`UsfMsgDisplayInfo::Finish` sends status 4 whenever it is ≥ 1, even with the
panel off [HAL 0x226d0]. `usf.display_state_from_backlight()` mirrors the
parser.

### 10.5 When it is sent

* **Start.** `SensorHal::Init` runs `Connect` (registry, §7), then
  `ProxMonitorService::Init`, then the sensor list, then
  `DisplayInfoService::Init` [HAL 0x2dd44..0x2ddc8]. `Init` does
  GetServer(display info), then sets a one-shot 500 ms timer [HAL
  0x2551c..0x25554]. The timer runs `ParseRegistryFile`. If at least one panel
  has an ALS, it then runs `UpdateSensorSetting` and the
  `DisplayStateMonitor` thread; otherwise it runs
  `DisplayStateMonitorSimple`, which only tracks panel 0's state and never
  sends [HAL 0x20850..0x208b8, 0x21be0].
* **`ParseRegistryFile`** [HAL 0x20930] reads the AoC registry (RegistryGet)
  for p = 0, 1, stopping at the first node without `attached_alsp`:
  1. `/dev/display_info/<p>`: `attached_alsp` (cheetah: `/dev/tmd3719/0`)
     and `panel_name`.
  2. `<alsp>`: `is_fac_cal` and `is_te2_node_enabled`.
  3. `itime_table_brightness` and the `histo_*` settings, from
     `<alsp>/<panel_name>/als` when that exists, else from `<alsp>/als`.
* **`UpdateSensorSetting`** [HAL 0x210e0] writes the `itime_table_brightness`
  string (cheetah: `100 200 300 400 2048`) to
  `/sys/class/backlight/panel<p>-backlight/als_table`. The stock panel driver
  then signals `brightness` only when the DBV crosses one of those values
  (**[I]**).
* **`DisplayStateMonitor`** [HAL 0x212b0] polls (`POLLPRI`, sysfs notify) the
  `brightness`, `state`, `local_hbm_mode` and `op_hz` files of each panel
  (and `te2_rate_hz` when enabled), plus the exit pipe. A sysfs file that has
  not been read yet polls as ready, so the first pass reads everything and
  sends the **initial report** (**[I]**: kernfs behaviour). Each file that
  fires updates the content and marks the panel changed [HAL
  0x216c0..0x21848]. Then, for each panel:
  1. The histogram runs while the status is on, HBM or LHBM, `histo_enable`
     is set and DBV ≤ `histo_max_brightness` (2047). The poll timeout is then
     `histo_polling_interval` (50 ms) [HAL 0x215f0..0x21640, 0x218ec..0x21930].
  2. Each tick: luma = `RequestHistogram(p)`, DBV read again, leakage =
     `LeakageCalculator::Calculate(luma, DBV)`. Leakage and luma are 0 while
     the histogram is inactive.
  3. When |leakage − last| ≥ max(`histo_min_change_value`,
     last·`histo_min_change_rate`) (0.01 and 1 %), the content is updated and
     the panel marked changed [HAL 0x21a64..0x21ab4].
  4. Every changed panel gets `SendInfoReq(p)` [HAL 0x21ad0..0x21afc].
* **`SendInfoReq(uint8 p)`** [HAL 0x21e00]: `p` is the panel index and
  becomes `display_id`. It returns 0 without sending when not connected
  [HAL 0x21f3c]. Otherwise it timestamps the content, adds it to a 250-entry
  history (dump only), copies it into the message and sends it.
* **After an AoC restart**, `Reconnect` looks the server up again and at once
  sends the last state of every panel, then re-reads the registry and rewrites
  `als_table` [HAL 0x25670..0x25780].

In short: once at start, then on each change of brightness, display state,
refresh rate, LHBM or op_hz, and up to 20 times a second while the histogram
leakage estimate moves by ≥ 1 %. The stock AoC log shows reports 30–60 ms
apart during a brightness ramp.

`LeakageCalculator` (registry comments, [HAL 0x26340]): leakage =
luma_term(luma) · dbv_term(DBV). Each term is picked by DBV range from
`histo_luma_eqN` / `histo_dbv_eqN` = `start-dbv end-dbv type coeffs…`, where
type 0 is c0 + c1·x + c2·x² + … and type 1 is c0 + c1·ln(x + c2). An optional
per-device calibration then scales it.

### 10.6 Panel info in the registry (`AddPanelInfosToRegistry`)

`SensorHal::Connect`: if `"/"` does not have `loaded=1`, it calls
`AddPanelInfosToRegistry` (errors are only logged), then `LoadRegistry` (the
`.reg` scripts, then `"/" loaded=1`) [HAL 0x2ed14..0x2ed68]. So the panel
properties go in **first**, with RegistrySet (id 22) to the registry server
through `RegistryHelper::SetProps` [HAL 0x41c80: `mov w1,#0x16`, registry UUID
0x16468]. The server creates the node (`AddNodeWithPath` [V 0x815e0]). The
script's `+/dev/display_info/0` block (`panel_max_rr=120`,
`attached_alsp=/dev/tmd3719/0`) then adds to the same node.

For p = 0, then 1: RegistrySet(`/dev/display_info/<p>`, 4 properties, 1 s
timeout) [HAL 0x313e8..0x31488; key table 0x51490]. A missing
`secondary-panel/panel_name` ends the loop quietly. A missing primary
`panel_name` returns error 7 [HAL 0x31504..0x31530].

| property | value | source |
|---|---|---|
| `panel_name` | `panel-b` for `boe-nt37290`, `nt37290`, `google-bigsurf`, `google-ct3b`, `google-ct3d`, `google-tk4b`, `google-tg4b`, `google-tg4c` and `google-fleb`; `panel-a` for anything else, including an empty name | first word of `/sys/class/drm/card0/device/primary-panel/panel_name` [HAL 0x386f0; map from the static initialiser 0x3dd20; default 0x38988..0x389c8] |
| `panel_serial` | first word of `.../primary-panel/serial_number`; `unknown` if it is missing or empty | [HAL 0x38ad0, 0x31320..0x31364] |
| `panel_is_original` | `1` if `IDisplay::getPanelCalibrationStatus` returns 0 (ORIGINAL), else `0`. The default is 2, so `0` when IDisplay is unavailable. | [HAL 0x31374..0x31474, IDisplay vtable +0xa0; the same call logs `panel_cal_status` in `PanelIsRlModule` 0x383a0]; enum names **[I]** |
| `panel_ext_info` | `0x` + first word of `.../primary-panel/panel_extinfo`; `0` if it is missing or empty | [HAL 0x38d10] |

`/dev/tmd3719/0/panel-b` exists in `cheetah_dvt.reg`: it holds the BOE
panel's `sync_delay_ns`, ALS scale, gain, `itime` and histogram/leakage tables
and prox burst delays and scales. This phone's panel is Samsung
`sdc-s6e3hc4` (kernel command line in the stock bugreport), so it maps to
`panel-a`. That subtree does not exist, so the base `/dev/tmd3719/0` values
apply. According to its log strings (§10.1), the AoC reads all four
properties, and it checks `panel_is_original` and `panel_serial` against the
factory calibration before applying it (**[I]**).
`usf.panel_registry_props()` builds the list, and
`load_registry(..., panel_props=...)` writes it before the scripts.

### 10.7 `ProxMonitorService`: minimum refresh while proximity is active

For sensors named `TMD3719 Proximity` or `TMD3733 Proximity` [HAL 0x2b388,
0x2b3ac], the service:

1. Reads `min_fps` from the prox node. Valid values are 30–120, otherwise 60;
   cheetah has 30 [HAL 0x2bbd8, 0x2bc84..0x2bcb0].
2. Finds the panel whose `attached_alsp` matches [HAL 0x2bcc0..0x2bf80].
3. Sends ReportSensorActive (id 37, empty payload) to the prox sensor's handle
   [HAL 0x2c24c].

The response, and later EVENTs, carry
`UsfMsgSensorActiveState {0: uint32 sensor handle, 1: uint32 state}`, with
state 0 kInactive, 1 kActive, 2 kUnknown [HAL 0x2c8e0; names 0x51450]. The
bugreport shows the matching `Received REPORT_SENSOR_ACTIVE_STATE_CHANGES
event from 'TMD3719 Proximity' with new state:kActive!`.

When the combined state changes [HAL 0x2ca74..0x2cacc, 0x2cb10]:

| prox | HAL calls on the display |
|---|---|
| active | `IDisplay::setMinIdleRefreshRate(min_fps)` (vtable +0x88) and `setRefreshRateThrottle(1000 ms)` (+0x90) |
| inactive | both with 0 |

Nothing about this goes to the AoC. **Requirement for Linux:** while proximity
is sampling, the panel must not idle below `min_fps` (30 Hz), because the prox
bursts are timed to frames (`burst_delay_*hz`). A panel that stays at a fixed
60/120 Hz meets it.

### 10.8 What a Linux client has to send

1. Optional, only when the registry is not loaded yet: RegistrySet
   `/dev/display_info/0` with `panel_registry_props(<first word of the panel
   name>, serial, is_original, ext_info)`, **before** the `.reg` scripts and
   `"/" loaded=1` (`load_registry(..., panel_props=...)`). For this phone,
   `panel_name` is `panel-a`. `panel_serial` and `panel_is_original=1`
   presumably decide whether the AoC applies the factory ALS/prox calibration
   (**[I]**).
2. After the registry is loaded and the sensors are enumerated:
   `D = get_server(UUID_DISPLAY_INFO)`.
3. At once, send `display_info(D, brightness=<DBV>, status=<1 on | 0 off |
   2 AOD>, refresh_rate=<60|120; 0 off; 30 LP>, op_hz=120, display_id=0)`,
   with leakage and luma 0 (no histogram). On this port the DBV is the
   `pixel-panel` backlight's `brightness`, which is written straight to DCS
   0x51. Send `op_hz=60` only if the panel runs in NS mode.
4. Send it again on every change of DBV, on/off/AOD, refresh rate or op_hz.
   After an AoC restart, look `D` up again and resend the current state, as
   `Reconnect` does.
5. Expect a RESP with status 0 and no payload.

### 10.9 Example bytes (generated by `usf.py`; server handle 5, req_id 7)

DisplayInfo `{brightness 372, status 1 (on), refresh_rate 60, op_hz 120,
leakage 1.432487}` (120 bytes):
```
  0000  10 00 00 00 00 00 00 00 08 00 0c 00 08 00 04 00   UsfMsg
  0010  08 00 00 00 08 00 00 00 01 00 00 00 58 00 00 00   type=1 REQ | data len 0x58
  0020  10 00 00 00 0c 00 14 00 10 00 0c 00 08 00 04 00   envelope
  0030  0c 00 00 00 10 00 00 00 05 00 00 00 07 00 00 00   handle=5 | req_id=7
  0040  e9 03 00 00 30 00 00 00 18 00 00 00 00 00 12 00   msg_id=1001 | payload 0x30 B | root→0x18 | vtable 18 B (7 slots)
  0050  18 00 14 00 07 00 10 00 00 00 0c 00 00 00 08 00   table 24 B; slot0@+20 slot1@+7 slot2@+16 slot3 - slot4@+12 slot5 - slot6@+8
  0060  12 00 00 00 00 00 00 01 78 00 00 00 bc 5b b7 3f   soffset | status=1 | op_hz=120 | leakage=1.432487
  0070  3c 00 00 00 74 01 00 00                           refresh_rate=60 | brightness=372
```
The HAL's builder places the fields in a different order, so the offsets in
the vtable differ; the decoded values are the same.
