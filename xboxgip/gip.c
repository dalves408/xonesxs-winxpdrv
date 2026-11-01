/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 dalves408 */
/*
 * GIP protocol handling.
 *
 * Startup follows what Windows does on the wire (seen in a Windows 11 USB
 * trace) and the xone Linux driver, which implements the same host side:
 *
 *   pad  -> announce (0x02)
 *   host -> identify request (0x04, no payload)
 *   pad  -> identify response, in acknowledged chunks
 *   host -> power on (0x05), LED (0x0A), auth done (0x06)
 *   pad  -> input (0x20)
 *
 * Packet layouts for input, rumble and the init packets follow the Linux
 * xpad driver. Header framing (varint lengths, chunks, acks) follows xone.
 */
#include "xboxgip.h"

#define GIP_OPT_CHUNK_START 0x40
#define GIP_OPT_CHUNK       0x80

/* Decoded GIP header: command, options, sequence, varint length [, varint chunk offset]. */
typedef struct _GIP_HEADER {
    UCHAR Command;
    UCHAR Options;
    UCHAR Sequence;
    ULONG Length;           /* payload bytes in this packet */
    ULONG ChunkOffset;      /* chunk start: total length; later chunks: offset */
    ULONG HeaderLength;
} GIP_HEADER, *PGIP_HEADER;

/* Templates; byte 2 (sequence) is filled in when sent. */
static const UCHAR GipPowerOn[]     = { GIP_CMD_POWER, GIP_OPT_INTERNAL, 0, 0x01, 0x00 };
static const UCHAR GipSInit[]       = { GIP_CMD_POWER, GIP_OPT_INTERNAL, 0, 0x0F, 0x06 };
static const UCHAR GipExtraInput[]  = { GIP_CMD_EXTRA_INPUT, 0x10, 0, 0x02, 0x07, 0x00 };
static const UCHAR GipLedOn[]       = { GIP_CMD_LED, GIP_OPT_INTERNAL, 0, 0x03, 0x00, 0x01, 0x14 };
static const UCHAR GipAuthDone[]    = { GIP_CMD_AUTHENTICATE, GIP_OPT_INTERNAL, 0, 0x02, 0x01, 0x00 };

typedef struct _GIP_INIT_PACKET {
    USHORT       ProductId;     /* 0 = every controller */
    const UCHAR *Data;
    ULONG        Length;
} GIP_INIT_PACKET;

/* Same packets and order as xpad's xboxone_init_packets[] for Microsoft pads. */
static const GIP_INIT_PACKET GipInitPackets[] = {
    { 0,      GipPowerOn,    sizeof(GipPowerOn) },
    { 0x02EA, GipSInit,      sizeof(GipSInit) },        /* One S, after Bluetooth use */
    { 0x0B00, GipSInit,      sizeof(GipSInit) },        /* Elite 2 */
    { 0x0B00, GipExtraInput, sizeof(GipExtraInput) },   /* Elite 2 paddles */
    { 0,      GipLedOn,      sizeof(GipLedOn) },
    { 0,      GipAuthDone,   sizeof(GipAuthDone) },     /* what Windows sends after auth */
};

/*
 * Windows mode: sequence numbers run 1..255 and never 0, as in xone
 * ("always greater than zero"). xpad mode: numbered from 0, as xpad does.
 */
static UCHAR
GipNextSequence(
    PDEVICE_CONTEXT Context
    )
{
    LONG sequence;

#if XGIP_XPAD_MODE
    sequence = (InterlockedIncrement(&Context->Sequence) - 1) & 0xFF;
#else
    do {
        sequence = InterlockedIncrement(&Context->Sequence) & 0xFF;
    } while (sequence == 0);
#endif

    return (UCHAR)sequence;
}

/* ------------------------------------------------------------------------- */
/* Init sequence: power on, LED, auth done - one packet at a time.           */
/* ------------------------------------------------------------------------- */

/* Sends the next applicable init packet, or ends the sequence. */
static VOID
GipSendNextInit(
    PDEVICE_CONTEXT Context
    )
{
    USHORT pid = Context->DeviceDescriptor.idProduct;
    UCHAR packet[GIP_MAX_PACKET];
    const GIP_INIT_PACKET *entry;

    for (;;) {
        while (Context->InitStep < RTL_NUMBER_OF(GipInitPackets)) {
            entry = &GipInitPackets[Context->InitStep++];
            if (entry->ProductId != 0 && entry->ProductId != pid) {
                continue;
            }

            RtlCopyMemory(packet, entry->Data, entry->Length);
            packet[2] = GipNextSequence(Context);
            if (NT_SUCCESS(XgipUsbWrite(Context, packet, entry->Length, TRUE))) {
                return;     /* GipInitPacketDone continues from the write completion */
            }
        }

        KdPrint(("xboxgip: init sequence sent\n"));

        /* Another start was requested while we were sending; run it again. */
        if (InterlockedExchange(&Context->InitRestart, 0) == 0) {
            break;
        }
        Context->InitAttempts++;
        KdPrint(("xboxgip: init attempt %u (queued)\n", Context->InitAttempts));
        Context->InitStep = 0;
    }

    InterlockedExchange(&Context->InitActive, 0);
}

VOID
GipStartInit(
    PDEVICE_CONTEXT Context
    )
{
    /* Don't interleave two sequences; rerun once the current one finishes. */
    if (InterlockedCompareExchange(&Context->InitActive, 1, 0) != 0) {
        InterlockedExchange(&Context->InitRestart, 1);
        return;
    }

    Context->InitAttempts++;
    KdPrint(("xboxgip: init attempt %u\n", Context->InitAttempts));
    Context->InitStep = 0;
    GipSendNextInit(Context);
}

VOID
GipInitPacketDone(
    PDEVICE_CONTEXT Context
    )
{
    GipSendNextInit(Context);
}

/*
 * Fallback for when the identify exchange stalls or the pad stays quiet:
 * once a second, (re)send the init sequence until input arrives.
 */
VOID
XgipEvtInitTimer(
    WDFTIMER Timer
    )
{
    PDEVICE_CONTEXT context = GetDeviceContext(WdfTimerGetParentObject(Timer));

    if (context->GotInput || context->InitAttempts >= XGIP_INIT_MAX_ATTEMPTS) {
        KdPrint(("xboxgip: init retries stopped (input %u, attempts %u)\n",
                 context->GotInput, context->InitAttempts));
        WdfTimerStop(Timer, FALSE);
        return;
    }

    GipStartInit(context);
}

/* ------------------------------------------------------------------------- */
/* Outgoing packets                                                           */
/* ------------------------------------------------------------------------- */

static VOID
GipSendIdentifyRequest(
    PDEVICE_CONTEXT Context
    )
{
    UCHAR packet[4];

    packet[0] = GIP_CMD_IDENTIFY;
    packet[1] = GIP_OPT_INTERNAL;
    packet[2] = GipNextSequence(Context);
    packet[3] = 0x00;           /* no payload */

    KdPrint(("xboxgip: identify request\n"));
    XgipUsbWrite(Context, packet, sizeof(packet), FALSE);
}

/*
 * Acknowledges a received packet (xone gip_acknowledge_pkt). AckedLength is
 * how many bytes of the message have arrived so far; Remaining is what is
 * still to come in a chunked transfer.
 */
static VOID
GipSendAck(
    PDEVICE_CONTEXT Context,
    const GIP_HEADER *Header,
    ULONG AckedLength,
    ULONG Remaining
    )
{
    UCHAR packet[13];

    RtlZeroMemory(packet, sizeof(packet));
    packet[0] = GIP_CMD_ACK;
    packet[1] = GIP_OPT_INTERNAL;
    packet[2] = Header->Sequence;   /* echo the sequence being acknowledged */
    packet[3] = 0x09;
    packet[5] = Header->Command;
    packet[6] = GIP_OPT_INTERNAL;
    packet[7] = (UCHAR)AckedLength;
    packet[8] = (UCHAR)(AckedLength >> 8);
    packet[11] = (UCHAR)Remaining;
    packet[12] = (UCHAR)(Remaining >> 8);

    XgipUsbWrite(Context, packet, sizeof(packet), FALSE);
}

NTSTATUS
GipSendRumble(
    PDEVICE_CONTEXT Context,
    UCHAR LeftMotor,
    UCHAR RightMotor,
    UCHAR LeftTriggerMotor,
    UCHAR RightTriggerMotor
    )
{
    UCHAR packet[13];

    packet[0] = GIP_CMD_RUMBLE;
    packet[1] = 0x00;
    packet[2] = GipNextSequence(Context);
    packet[3] = 0x09;
    packet[4] = 0x00;
    packet[5] = 0x0F;           /* all four motors */
    packet[6] = LeftTriggerMotor;
    packet[7] = RightTriggerMotor;
    packet[8] = LeftMotor;
    packet[9] = RightMotor;
    packet[10] = 0xFF;          /* on period */
    packet[11] = 0x00;          /* off period */
    packet[12] = 0xFF;          /* repeat count */

    return XgipUsbWrite(Context, packet, sizeof(packet), FALSE);
}

/* ------------------------------------------------------------------------- */
/* Input state                                                                */
/* ------------------------------------------------------------------------- */

/* Stores the new state; if it changed, marks it dirty and feeds a pending HID read. */
static VOID
GipCommitState(
    PDEVICE_CONTEXT Context,
    const XGIP_STATE *NewState
    )
{
    BOOLEAN changed;

    WdfSpinLockAcquire(Context->StateLock);
    changed = (RtlCompareMemory(&Context->State, NewState, sizeof(*NewState)) != sizeof(*NewState));
    if (changed) {
        Context->State = *NewState;
        Context->ReportDirty = TRUE;
        Context->PacketNumber++;
    }
    WdfSpinLockRelease(Context->StateLock);

    if (changed) {
        XgipHidCompletePendingRead(Context);
    }
}

/* Payload of a 0x20 input packet (header already stripped). */
static VOID
GipParseInput(
    PDEVICE_CONTEXT Context,
    const UCHAR *p,
    ULONG Length
    )
{
    XGIP_STATE state;
    USHORT buttons = 0;

    if (p[0] & 0x04) buttons |= XGIP_BTN_START;
    if (p[0] & 0x08) buttons |= XGIP_BTN_BACK;
    if (p[0] & 0x10) buttons |= XGIP_BTN_A;
    if (p[0] & 0x20) buttons |= XGIP_BTN_B;
    if (p[0] & 0x40) buttons |= XGIP_BTN_X;
    if (p[0] & 0x80) buttons |= XGIP_BTN_Y;

    if (p[1] & 0x01) buttons |= XGIP_BTN_DPAD_UP;
    if (p[1] & 0x02) buttons |= XGIP_BTN_DPAD_DOWN;
    if (p[1] & 0x04) buttons |= XGIP_BTN_DPAD_LEFT;
    if (p[1] & 0x08) buttons |= XGIP_BTN_DPAD_RIGHT;
    if (p[1] & 0x10) buttons |= XGIP_BTN_LSHOULDER;
    if (p[1] & 0x20) buttons |= XGIP_BTN_RSHOULDER;
    if (p[1] & 0x40) buttons |= XGIP_BTN_LTHUMB;
    if (p[1] & 0x80) buttons |= XGIP_BTN_RTHUMB;

    /* Guide arrives in its own packet; keep its last value. */
    WdfSpinLockAcquire(Context->StateLock);
    state = Context->State;
    WdfSpinLockRelease(Context->StateLock);

    state.Buttons = (USHORT)((state.Buttons & XGIP_BTN_GUIDE) | buttons);

    state.LeftTriggerRaw  = (USHORT)((p[2] | (p[3] << 8)) & 0x3FF);
    state.RightTriggerRaw = (USHORT)((p[4] | (p[5] << 8)) & 0x3FF);
    state.LeftTrigger     = (UCHAR)(state.LeftTriggerRaw >> 2);
    state.RightTrigger    = (UCHAR)(state.RightTriggerRaw >> 2);

    state.ThumbLX = (SHORT)(p[6]  | (p[7]  << 8));
    state.ThumbLY = (SHORT)(p[8]  | (p[9]  << 8));
    state.ThumbRX = (SHORT)(p[10] | (p[11] << 8));
    state.ThumbRY = (SHORT)(p[12] | (p[13] << 8));

    /*
     * Series X|S: Share sits 26 bytes from the end of the whole packet (xpad
     * MAP_SHARE_OFFSET); with the 4-byte header that is payload[Length - 26].
     */
    if (Context->HasShareButton && Length >= GIP_INPUT_PAYLOAD_MIN + 26) {
        state.Share = (p[Length - 26] & 0x01) ? TRUE : FALSE;
    }

    GipCommitState(Context, &state);
}

static VOID
GipParseGuide(
    PDEVICE_CONTEXT Context,
    BOOLEAN Pressed
    )
{
    XGIP_STATE state;

    WdfSpinLockAcquire(Context->StateLock);
    state = Context->State;
    WdfSpinLockRelease(Context->StateLock);

    if (Pressed) {
        state.Buttons |= XGIP_BTN_GUIDE;
    } else {
        state.Buttons = (USHORT)(state.Buttons & ~XGIP_BTN_GUIDE);
    }

    GipCommitState(Context, &state);
}

/* ------------------------------------------------------------------------- */
/* Incoming packets                                                           */
/* ------------------------------------------------------------------------- */

/* Little-endian base-128 varint, at most 4 bytes. Returns bytes used, 0 if truncated. */
static ULONG
GipDecodeVarint(
    const UCHAR *Data,
    ULONG Length,
    PULONG Value
    )
{
    ULONG i;

    *Value = 0;
    for (i = 0; i < 4 && i < Length; i++) {
        *Value |= (ULONG)(Data[i] & 0x7F) << (i * 7);
        if (!(Data[i] & 0x80)) {
            return i + 1;
        }
    }

    return 0;
}

static BOOLEAN
GipDecodeHeader(
    const UCHAR *Data,
    ULONG Length,
    PGIP_HEADER Header
    )
{
    ULONG used;
    ULONG offset = 3;

    if (Length < GIP_HEADER_SIZE) {
        return FALSE;
    }

    Header->Command = Data[0];
    Header->Options = Data[1];
    Header->Sequence = Data[2];
    Header->ChunkOffset = 0;

    used = GipDecodeVarint(Data + offset, Length - offset, &Header->Length);
    if (used == 0) {
        return FALSE;
    }
    offset += used;

    if (Header->Options & GIP_OPT_CHUNK) {
        used = GipDecodeVarint(Data + offset, Length - offset, &Header->ChunkOffset);
        if (used == 0) {
            return FALSE;
        }
        offset += used;
    }

    Header->HeaderLength = offset;
    return (offset + Header->Length <= Length);
}

/* Handles one complete (non-chunked, or fully reassembled) message. */
static VOID
GipDispatch(
    PDEVICE_CONTEXT Context,
    const GIP_HEADER *Header,
    const UCHAR *Payload,
    ULONG Length
    )
{
    switch (Header->Command) {
    case GIP_CMD_ANNOUNCE:
        /* First thing the pad sends; it waits to be identified and powered on. */
        KdPrint(("xboxgip: announce\n"));
        GipSendIdentifyRequest(Context);
        break;

    case GIP_CMD_IDENTIFY:
        KdPrint(("xboxgip: identify complete\n"));
        GipStartInit(Context);
        break;

    case GIP_CMD_GUIDE:
        Context->GotInput = TRUE;
        if (Length >= 1) {
            GipParseGuide(Context, (Payload[0] & 0x03) ? TRUE : FALSE);
        }
        break;

    case GIP_CMD_INPUT:
        Context->GotInput = TRUE;
        if (Length >= GIP_INPUT_PAYLOAD_MIN) {
            GipParseInput(Context, Payload, Length);
        }
        break;

    default:
        /* Status/heartbeat and anything else: nothing to do yet. */
        break;
    }
}

static VOID
GipProcessPacket(
    PDEVICE_CONTEXT Context,
    PGIP_HEADER Header,
    const UCHAR *Payload
    )
{
    ULONG offset = Header->ChunkOffset;
    ULONG acked;

    /* The first chunk carries the total message length in its offset field. */
    if (Header->Options & GIP_OPT_CHUNK_START) {
        Context->ChunkTotal = Header->ChunkOffset;
        offset = 0;
    }

    /* The pad resends anything flagged for acknowledgement until it gets one. */
    if (Header->Options & GIP_OPT_ACK) {
        acked = offset + Header->Length;
        GipSendAck(Context, Header, acked,
                   ((Header->Options & GIP_OPT_CHUNK) && Context->ChunkTotal > acked) ?
                       Context->ChunkTotal - acked : 0);
    }

    if (Header->Options & GIP_OPT_CHUNK) {
        /* Only the identify message arrives chunked; its contents aren't needed. */
        if (Header->Length == 0) {
            /* An empty chunk ends the transfer. */
            Context->ChunkTotal = 0;
            GipDispatch(Context, Header, NULL, 0);
        }
        return;
    }

    GipDispatch(Context, Header, Payload, Header->Length);
}

/* One USB transfer can hold several GIP packets back to back. */
VOID
GipProcessTransfer(
    PDEVICE_CONTEXT Context,
    const UCHAR *Data,
    ULONG Length
    )
{
    GIP_HEADER header;

    Context->RxCount++;

#if DBG
    /* Log everything except the steady input stream, which is logged only at first. */
    if (Data[0] != GIP_CMD_INPUT || Context->RxCount <= 8) {
        UCHAR b[16];
        ULONG n = (Length < sizeof(b)) ? Length : sizeof(b);

        RtlZeroMemory(b, sizeof(b));
        RtlCopyMemory(b, Data, n);
        KdPrint(("xboxgip: <- #%u len %u: %02X %02X %02X %02X | %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                 Context->RxCount, Length, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
                 b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]));
    }
#endif

    while (Length > 0) {
        if (!GipDecodeHeader(Data, Length, &header)) {
            KdPrint(("xboxgip: malformed GIP packet (%u bytes left)\n", Length));
            return;
        }

        GipProcessPacket(Context, &header, Data + header.HeaderLength);

        Data += header.HeaderLength + header.Length;
        Length -= header.HeaderLength + header.Length;
    }
}
