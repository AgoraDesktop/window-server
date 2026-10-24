# CLIPBOARD-PLAN — a selection that crosses applications

**STATUS: the first two artifacts exist.** This note is the decision; `protocol/agora-shell.xml` version 7 is the wire;
`src/shell.c` implements the compositor's half behind a test that two real clients pass bytes through. The AppKit side
(`NSPasteboard` over this, and the seam doors it needs) is **not written** — see §7.

## 0. What the existing pasteboard is, and is not

`libagora-app/NSPasteboard.m` is a working in-process pasteboard: items, types, `changeCount`, the reader and writer
doors, `NSTextView`'s copy and paste on top of it. Its header says the honest thing — that the implementation is
process-local and session integration is later work. Later is now, and this note is about the part that is missing: **two
applications, one selection**.

## 1. The three decisions

### 1.1 The compositor owns the selection

A selection is a fact about the SEAT — which client is in front, which client may be pasted into — and the window server
already owns the seat, the focus and the socket. A separate session daemon would be a second process to start,
supervise and speak yet another protocol to, and this tree has no session bus to hang it on.

The consequence is stated so nobody is surprised by it: **a selection dies with its owner.** A client that copies text
and exits leaves nothing behind. That is what a Mac does when the application that owned the selection quits, and it is
the right default here — a clipboard *manager* (a daemon that keeps a copy) is a separate component that would sit
between the compositor and the client, and the protocol below is shaped so it can: it offers a selection, it does not
promise one forever.

### 1.2 The transfer is an offer, not a copy

The owner advertises MIME types; a reader asks for one and receives a **file descriptor** to read from. Nothing passes
through the compositor: no bytes, no size limit, no copy of a 50 MB image, and no way for a hostile owner to make the
compositor allocate the payload.

**What the compositor does pass is the fd, and only the fd.** On a request it creates a pipe, hands the WRITE end to the
owner (`selection_requested`) and the READ end to the reader (`selection_sent`). The owner writes and closes; the reader
sees EOF. The compositor is never in the data path and never blocks on either end.

### 1.3 "Nonblocking" means a polled promise with a reason

`NSPasteboard` on the Mac may block: reading it can wait on another process. This seam has no blocking calls and must
not re-enter the event loop, so the contract is a **polled promise**:

- The selection's availability is announced (`selection_offer`, one event per type) and its disappearance is announced
  (`selection_lost`) — a reader knows what exists without asking.
- A request that cannot be served is answered **immediately and specifically** (`selection_refused` with a reason), rather
  than by silence: *no owner*, *not that type*, *the owner is gone*. A paste that silently becomes the empty string is
  the failure people actually hate, and this is the whole reason the reason exists.
- A request that can be served is answered with an fd **before the data is written**, because the promise is the fd.
  EOF is the end of the payload; a reader that must not block reads with `O_NONBLOCK` and treats `EAGAIN` as "not yet".

## 2. The wire — `agora_shell` version 7

Five events and three requests, all on the existing global (there is no new object: a selection is not a thing a client
can own several of).

```xml
<request name="set_selection" since="7">
  <arg name="types" type="array" summary="MIME types this client can serve"/>
</request>
<request name="clear_selection" since="7"/>
<request name="request_selection" since="7">
  <arg name="mime_type" type="string"/>
</request>

<event name="selection_offer" since="7"><arg name="mime_type" type="string"/></event>
<event name="selection_lost" since="7"/>
<event name="selection_requested" since="7">
  <arg name="mime_type" type="string"/><arg name="fd" type="fd"/>
</event>
<event name="selection_sent" since="7">
  <arg name="mime_type" type="string"/><arg name="fd" type="fd"/>
</event>
<event name="selection_refused" since="7">
  <arg name="mime_type" type="string"/><arg name="reason" type="uint"/>
</event>
```

### Why the version moved and what it costs

The shell's version is the cap its children inherit, so raising it to 7 hands out version 7 shells to anyone asking —
and the events below are *unsolicited*, which is the part that needs care: a client bound at 7 that never asked about
the clipboard is still sent `selection_offer` when somebody copies. That is the same shape `agora_output` already has
(an event-only interface), and clients ignore what they do not handle. Everything new is `since="7"`, so a client bound
below that sees nothing.

### Errors and refusals

`selection_refused.reason` is an enum on the wire, not a string:

| value | name | what the reader should do |
|---|---|---|
| 0 | `no_owner` | nothing is on the clipboard; paste the empty string if that is what the user asked |
| 1 | `wrong_type` | an owner exists but cannot serve this type; paste nothing rather than a lie |
| 2 | `owner_gone` | the owner disappeared between the offer and the request; treat as `no_owner` |

## 3. Ownership, lifetime, and who is told what

- `set_selection` REPLACES whatever was there, from whichever client. **No focus check.** A background application may
  own the selection — that is what "copy from a terminal that lost focus" means — and gating it on focus would be a
  policy nobody asked for.
- The compositor records the owner's `wl_resource` and its types. When that resource is destroyed, the selection goes
  with it and every client hears `selection_lost`.
- `clear_selection` is the explicit version of the same thing.
- A second `set_selection` from a different client is announced to everyone as `selection_lost` followed by the new
  `selection_offer`s, in that order, so a reader's state machine has one transition to handle.

## 4. What the compositor never does

- It never reads the data, so it cannot validate it.
- It never buffers it, so it cannot serve a reader after the owner has gone.
- It never blocks: both fds are handed over and closed by the compositor immediately, and the pipe's lifetime is the two
  clients'.
- It never grows without bound: one owner, one type list, and the list is capped (`AWS_SELECTION_MAX_TYPES`, 32) with the
  excess refused rather than truncated silently.

## 5. The seam (planned, not written)

`platform/AGWindowServer.h` grows the same three requests and five events as C doors:

```c
int  AGWindowServerSetSelection(AGWindowServer *server, const char *const *types, int count);
void AGWindowServerClearSelection(AGWindowServer *server);
int  AGWindowServerRequestSelection(AGWindowServer *server, const char *mime_type);
/* handlers */
typedef void (*AGSelectionOfferFunction)(void *context, const char *mime_type);
typedef void (*AGSelectionLostFunction)(void *context);
typedef void (*AGSelectionRequestedFunction)(void *context, const char *mime_type, int fd);
typedef void (*AGSelectionSentFunction)(void *context, const char *mime_type, int fd);
typedef void (*AGSelectionRefusedFunction)(void *context, const char *mime_type, AGSelectionRefusal why);
```

The owner's callback writes into the fd and closes it; the reader's reads and closes it. Closing is the client's job in
both directions, which is why the callbacks own the fd rather than the server.

## 6. The AppKit surface (planned, not written)

`NSPasteboard` on top of §5, non-blocking, with the reasons preserved:

- `-changeCount` moves on every `selection_offer`/`selection_lost`, so the existing in-process polling idiom keeps working
  across applications.
- `-types` answers the current offer without a round trip (the events are the cache).
- `-dataForType:` answers `nil` for `wrong_type`/`no_owner`, and **never blocks**; a reader that wants the async shape
  gets the fd through a private door and reads it with `O_NONBLOCK`.
- `-setData:forType:` / `-declareTypes:` drive `set_selection`; a file-backed owner writes on `selection_requested`.

## 7. Delivery order

1. **This note and the protocol** (done).
2. **The compositor's half**, with a test that two real clients pass bytes through (done — `tests/clipboard.c`).
3. **The seam doors** in `platform/AGWindowServer-Wayland.c`, plus their `AGWindowServer.h` declarations.
4. **`NSPasteboard` over the seam**, which is the piece `libagora-app#13` describes, and the piece that must keep the
   distinction between "nothing there" and "not yet".
5. **A clipboard manager**, if one is ever wanted — it would be an ordinary client that offers back what it received,
   which is exactly what this protocol allows and no more.
