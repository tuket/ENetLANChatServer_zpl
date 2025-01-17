#include "enet.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ----- callbacks.c -----

static ENetCallbacks callbacks = { malloc, free, abort };

int
enet_initialize_with_callbacks (ENetVersion version, const ENetCallbacks * inits)
{
   if (version < ENET_VERSION_CREATE (1, 3, 0))
     return -1;

   if (inits -> malloc != NULL || inits -> free != NULL)
   {
      if (inits -> malloc == NULL || inits -> free == NULL)
        return -1;

      callbacks.malloc = inits -> malloc;
      callbacks.free = inits -> free;
   }
      
   if (inits -> no_memory != NULL)
     callbacks.no_memory = inits -> no_memory;

   return enet_initialize ();
}

ENetVersion
enet_linked_version (void)
{
    return ENET_VERSION;
}
           
void *
enet_malloc (size_t size)
{
   void * memory = callbacks.malloc (size);

   if (memory == NULL)
     callbacks.no_memory ();

   return memory;
}

void
enet_free (void * memory)
{
   callbacks.free (memory);
}

// ----- compress.c -----

typedef struct _ENetSymbol
{
    /* binary indexed tree of symbols */
    enet_uint8 value;
    enet_uint8 count;
    enet_uint16 under;
    enet_uint16 left, right;

    /* context defined by this symbol */
    enet_uint16 symbols;
    enet_uint16 escapes;
    enet_uint16 total;
    enet_uint16 parent; 
} ENetSymbol;

/* adaptation constants tuned aggressively for small packet sizes rather than large file compression */
enum
{
    ENET_RANGE_CODER_TOP    = 1<<24,
    ENET_RANGE_CODER_BOTTOM = 1<<16,

    ENET_CONTEXT_SYMBOL_DELTA = 3,
    ENET_CONTEXT_SYMBOL_MINIMUM = 1,
    ENET_CONTEXT_ESCAPE_MINIMUM = 1,

    ENET_SUBCONTEXT_ORDER = 2,
    ENET_SUBCONTEXT_SYMBOL_DELTA = 2,
    ENET_SUBCONTEXT_ESCAPE_DELTA = 5
};

/* context exclusion roughly halves compression speed, so disable for now */
#undef ENET_CONTEXT_EXCLUSION

typedef struct _ENetRangeCoder
{
    /* only allocate enough symbols for reasonable MTUs, would need to be larger for large file compression */
    ENetSymbol symbols[4096];
} ENetRangeCoder;

void *
enet_range_coder_create (void)
{
    ENetRangeCoder * rangeCoder = (ENetRangeCoder *) enet_malloc (sizeof (ENetRangeCoder));
    if (rangeCoder == NULL)
      return NULL;

    return rangeCoder;
}

void
enet_range_coder_destroy (void * context)
{
    ENetRangeCoder * rangeCoder = (ENetRangeCoder *) context;
    if (rangeCoder == NULL)
      return;

    enet_free (rangeCoder);
}

#define ENET_SYMBOL_CREATE(symbol, value_, count_) \
{ \
    symbol = & rangeCoder -> symbols [nextSymbol ++]; \
    symbol -> value = value_; \
    symbol -> count = count_; \
    symbol -> under = count_; \
    symbol -> left = 0; \
    symbol -> right = 0; \
    symbol -> symbols = 0; \
    symbol -> escapes = 0; \
    symbol -> total = 0; \
    symbol -> parent = 0; \
}

#define ENET_CONTEXT_CREATE(context, escapes_, minimum) \
{ \
    ENET_SYMBOL_CREATE (context, 0, 0); \
    (context) -> escapes = escapes_; \
    (context) -> total = escapes_ + 256*minimum; \
    (context) -> symbols = 0; \
}

static enet_uint16
enet_symbol_rescale (ENetSymbol * symbol)
{
    enet_uint16 total = 0;
    for (;;)
    {
        symbol -> count -= symbol->count >> 1;
        symbol -> under = symbol -> count;
        if (symbol -> left)
          symbol -> under += enet_symbol_rescale (symbol + symbol -> left);
        total += symbol -> under;
        if (! symbol -> right) break;
        symbol += symbol -> right;
    } 
    return total;
}

#define ENET_CONTEXT_RESCALE(context, minimum) \
{ \
    (context) -> total = (context) -> symbols ? enet_symbol_rescale ((context) + (context) -> symbols) : 0; \
    (context) -> escapes -= (context) -> escapes >> 1; \
    (context) -> total += (context) -> escapes + 256*minimum; \
}

#define ENET_RANGE_CODER_OUTPUT(value) \
{ \
    if (outData >= outEnd) \
      return 0; \
    * outData ++ = value; \
}

#define ENET_RANGE_CODER_ENCODE(under, count, total) \
{ \
    encodeRange /= (total); \
    encodeLow += (under) * encodeRange; \
    encodeRange *= (count); \
    for (;;) \
    { \
        if((encodeLow ^ (encodeLow + encodeRange)) >= ENET_RANGE_CODER_TOP) \
        { \
            if(encodeRange >= ENET_RANGE_CODER_BOTTOM) break; \
            encodeRange = -encodeLow & (ENET_RANGE_CODER_BOTTOM - 1); \
        } \
        ENET_RANGE_CODER_OUTPUT (encodeLow >> 24); \
        encodeRange <<= 8; \
        encodeLow <<= 8; \
    } \
}

#define ENET_RANGE_CODER_FLUSH \
{ \
    while (encodeLow) \
    { \
        ENET_RANGE_CODER_OUTPUT (encodeLow >> 24); \
        encodeLow <<= 8; \
    } \
}

#define ENET_RANGE_CODER_FREE_SYMBOLS \
{ \
    if (nextSymbol >= sizeof (rangeCoder -> symbols) / sizeof (ENetSymbol) - ENET_SUBCONTEXT_ORDER ) \
    { \
        nextSymbol = 0; \
        ENET_CONTEXT_CREATE (root, ENET_CONTEXT_ESCAPE_MINIMUM, ENET_CONTEXT_SYMBOL_MINIMUM); \
        predicted = 0; \
        order = 0; \
    } \
}

#define ENET_CONTEXT_ENCODE(context, symbol_, value_, under_, count_, update, minimum) \
{ \
    under_ = value*minimum; \
    count_ = minimum; \
    if (! (context) -> symbols) \
    { \
        ENET_SYMBOL_CREATE (symbol_, value_, update); \
        (context) -> symbols = symbol_ - (context); \
    } \
    else \
    { \
        ENetSymbol * node = (context) + (context) -> symbols; \
        for (;;) \
        { \
            if (value_ < node -> value) \
            { \
                node -> under += update; \
                if (node -> left) { node += node -> left; continue; } \
                ENET_SYMBOL_CREATE (symbol_, value_, update); \
                node -> left = symbol_ - node; \
            } \
            else \
            if (value_ > node -> value) \
            { \
                under_ += node -> under; \
                if (node -> right) { node += node -> right; continue; } \
                ENET_SYMBOL_CREATE (symbol_, value_, update); \
                node -> right = symbol_ - node; \
            } \
            else \
            { \
                count_ += node -> count; \
                under_ += node -> under - node -> count; \
                node -> under += update; \
                node -> count += update; \
                symbol_ = node; \
            } \
            break; \
        } \
    } \
}

#ifdef ENET_CONTEXT_EXCLUSION
static const ENetSymbol emptyContext = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };

#define ENET_CONTEXT_WALK(context, body) \
{ \
    const ENetSymbol * node = (context) + (context) -> symbols; \
    const ENetSymbol * stack [256]; \
    size_t stackSize = 0; \
    while (node -> left) \
    { \
        stack [stackSize ++] = node; \
        node += node -> left; \
    } \
    for (;;) \
    { \
        body; \
        if (node -> right) \
        { \
            node += node -> right; \
            while (node -> left) \
            { \
                stack [stackSize ++] = node; \
                node += node -> left; \
            } \
        } \
        else \
        if (stackSize <= 0) \
            break; \
        else \
            node = stack [-- stackSize]; \
    } \
}

#define ENET_CONTEXT_ENCODE_EXCLUDE(context, value_, under, total, minimum) \
ENET_CONTEXT_WALK(context, { \
    if (node -> value != value_) \
    { \
        enet_uint16 parentCount = rangeCoder -> symbols [node -> parent].count + minimum; \
        if (node -> value < value_) \
          under -= parentCount; \
        total -= parentCount; \
    } \
})
#endif

size_t
enet_range_coder_compress (void * context, const ENetBuffer * inBuffers, size_t inBufferCount, size_t inLimit, enet_uint8 * outData, size_t outLimit)
{
    ENetRangeCoder * rangeCoder = (ENetRangeCoder *) context;
    enet_uint8 * outStart = outData, * outEnd = & outData [outLimit];
    const enet_uint8 * inData, * inEnd;
    enet_uint32 encodeLow = 0, encodeRange = ~0;
    ENetSymbol * root;
    enet_uint16 predicted = 0;
    size_t order = 0, nextSymbol = 0;

    if (rangeCoder == NULL || inBufferCount <= 0 || inLimit <= 0)
      return 0;

    inData = (const enet_uint8 *) inBuffers -> data;
    inEnd = & inData [inBuffers -> dataLength];
    inBuffers ++;
    inBufferCount --;

    ENET_CONTEXT_CREATE (root, ENET_CONTEXT_ESCAPE_MINIMUM, ENET_CONTEXT_SYMBOL_MINIMUM);

    for (;;)
    {
        ENetSymbol * subcontext, * symbol;
#ifdef ENET_CONTEXT_EXCLUSION
        const ENetSymbol * childContext = & emptyContext;
#endif
        enet_uint8 value;
        enet_uint16 count, under, * parent = & predicted, total;
        if (inData >= inEnd)
        {
            if (inBufferCount <= 0)
              break;
            inData = (const enet_uint8 *) inBuffers -> data;
            inEnd = & inData [inBuffers -> dataLength];
            inBuffers ++;
            inBufferCount --;
        }
        value = * inData ++;
    
        for (subcontext = & rangeCoder -> symbols [predicted]; 
             subcontext != root; 
#ifdef ENET_CONTEXT_EXCLUSION
             childContext = subcontext, 
#endif
                subcontext = & rangeCoder -> symbols [subcontext -> parent])
        {
            ENET_CONTEXT_ENCODE (subcontext, symbol, value, under, count, ENET_SUBCONTEXT_SYMBOL_DELTA, 0);
            * parent = symbol - rangeCoder -> symbols;
            parent = & symbol -> parent;
            total = subcontext -> total;
#ifdef ENET_CONTEXT_EXCLUSION
            if (childContext -> total > ENET_SUBCONTEXT_SYMBOL_DELTA + ENET_SUBCONTEXT_ESCAPE_DELTA)
              ENET_CONTEXT_ENCODE_EXCLUDE (childContext, value, under, total, 0);
#endif
            if (count > 0)
            {
                ENET_RANGE_CODER_ENCODE (subcontext -> escapes + under, count, total);
            }
            else
            {
                if (subcontext -> escapes > 0 && subcontext -> escapes < total) 
                    ENET_RANGE_CODER_ENCODE (0, subcontext -> escapes, total); 
                subcontext -> escapes += ENET_SUBCONTEXT_ESCAPE_DELTA;
                subcontext -> total += ENET_SUBCONTEXT_ESCAPE_DELTA;
            }
            subcontext -> total += ENET_SUBCONTEXT_SYMBOL_DELTA;
            if (count > 0xFF - 2*ENET_SUBCONTEXT_SYMBOL_DELTA || subcontext -> total > ENET_RANGE_CODER_BOTTOM - 0x100)
              ENET_CONTEXT_RESCALE (subcontext, 0);
            if (count > 0) goto nextInput;
        }

        ENET_CONTEXT_ENCODE (root, symbol, value, under, count, ENET_CONTEXT_SYMBOL_DELTA, ENET_CONTEXT_SYMBOL_MINIMUM);
        * parent = symbol - rangeCoder -> symbols;
        parent = & symbol -> parent;
        total = root -> total;
#ifdef ENET_CONTEXT_EXCLUSION
        if (childContext -> total > ENET_SUBCONTEXT_SYMBOL_DELTA + ENET_SUBCONTEXT_ESCAPE_DELTA)
          ENET_CONTEXT_ENCODE_EXCLUDE (childContext, value, under, total, ENET_CONTEXT_SYMBOL_MINIMUM); 
#endif
        ENET_RANGE_CODER_ENCODE (root -> escapes + under, count, total);
        root -> total += ENET_CONTEXT_SYMBOL_DELTA; 
        if (count > 0xFF - 2*ENET_CONTEXT_SYMBOL_DELTA + ENET_CONTEXT_SYMBOL_MINIMUM || root -> total > ENET_RANGE_CODER_BOTTOM - 0x100)
          ENET_CONTEXT_RESCALE (root, ENET_CONTEXT_SYMBOL_MINIMUM);

    nextInput:
        if (order >= ENET_SUBCONTEXT_ORDER) 
          predicted = rangeCoder -> symbols [predicted].parent;
        else 
          order ++;
        ENET_RANGE_CODER_FREE_SYMBOLS;
    }

    ENET_RANGE_CODER_FLUSH;

    return (size_t) (outData - outStart);
}

#define ENET_RANGE_CODER_SEED \
{ \
    if (inData < inEnd) decodeCode |= * inData ++ << 24; \
    if (inData < inEnd) decodeCode |= * inData ++ << 16; \
    if (inData < inEnd) decodeCode |= * inData ++ << 8; \
    if (inData < inEnd) decodeCode |= * inData ++; \
}

#define ENET_RANGE_CODER_READ(total) ((decodeCode - decodeLow) / (decodeRange /= (total)))

#define ENET_RANGE_CODER_DECODE(under, count, total) \
{ \
    decodeLow += (under) * decodeRange; \
    decodeRange *= (count); \
    for (;;) \
    { \
        if((decodeLow ^ (decodeLow + decodeRange)) >= ENET_RANGE_CODER_TOP) \
        { \
            if(decodeRange >= ENET_RANGE_CODER_BOTTOM) break; \
            decodeRange = -decodeLow & (ENET_RANGE_CODER_BOTTOM - 1); \
        } \
        decodeCode <<= 8; \
        if (inData < inEnd) \
          decodeCode |= * inData ++; \
        decodeRange <<= 8; \
        decodeLow <<= 8; \
    } \
}

#define ENET_CONTEXT_DECODE(context, symbol_, code, value_, under_, count_, update, minimum, createRoot, visitNode, createRight, createLeft) \
{ \
    under_ = 0; \
    count_ = minimum; \
    if (! (context) -> symbols) \
    { \
        createRoot; \
    } \
    else \
    { \
        ENetSymbol * node = (context) + (context) -> symbols; \
        for (;;) \
        { \
            enet_uint16 after = under_ + node -> under + (node -> value + 1)*minimum, before = node -> count + minimum; \
            visitNode; \
            if (code >= after) \
            { \
                under_ += node -> under; \
                if (node -> right) { node += node -> right; continue; } \
                createRight; \
            } \
            else \
            if (code < after - before) \
            { \
                node -> under += update; \
                if (node -> left) { node += node -> left; continue; } \
                createLeft; \
            } \
            else \
            { \
                value_ = node -> value; \
                count_ += node -> count; \
                under_ = after - before; \
                node -> under += update; \
                node -> count += update; \
                symbol_ = node; \
            } \
            break; \
        } \
    } \
}

#define ENET_CONTEXT_TRY_DECODE(context, symbol_, code, value_, under_, count_, update, minimum, exclude) \
ENET_CONTEXT_DECODE (context, symbol_, code, value_, under_, count_, update, minimum, return 0, exclude (node -> value, after, before), return 0, return 0)

#define ENET_CONTEXT_ROOT_DECODE(context, symbol_, code, value_, under_, count_, update, minimum, exclude) \
ENET_CONTEXT_DECODE (context, symbol_, code, value_, under_, count_, update, minimum, \
    { \
        value_ = code / minimum; \
        under_ = code - code%minimum; \
        ENET_SYMBOL_CREATE (symbol_, value_, update); \
        (context) -> symbols = symbol_ - (context); \
    }, \
    exclude (node -> value, after, before), \
    { \
        value_ = node->value + 1 + (code - after)/minimum; \
        under_ = code - (code - after)%minimum; \
        ENET_SYMBOL_CREATE (symbol_, value_, update); \
        node -> right = symbol_ - node; \
    }, \
    { \
        value_ = node->value - 1 - (after - before - code - 1)/minimum; \
        under_ = code - (after - before - code - 1)%minimum; \
        ENET_SYMBOL_CREATE (symbol_, value_, update); \
        node -> left = symbol_ - node; \
    }) \

#ifdef ENET_CONTEXT_EXCLUSION
typedef struct _ENetExclude
{
    enet_uint8 value;
    enet_uint16 under;
} ENetExclude;

#define ENET_CONTEXT_DECODE_EXCLUDE(context, total, minimum) \
{ \
    enet_uint16 under = 0; \
    nextExclude = excludes; \
    ENET_CONTEXT_WALK (context, { \
        under += rangeCoder -> symbols [node -> parent].count + minimum; \
        nextExclude -> value = node -> value; \
        nextExclude -> under = under; \
        nextExclude ++; \
    }); \
    total -= under; \
}

#define ENET_CONTEXT_EXCLUDED(value_, after, before) \
{ \
    size_t low = 0, high = nextExclude - excludes; \
    for(;;) \
    { \
        size_t mid = (low + high) >> 1; \
        const ENetExclude * exclude = & excludes [mid]; \
        if (value_ < exclude -> value) \
        { \
            if (low + 1 < high) \
            { \
                high = mid; \
                continue; \
            } \
            if (exclude > excludes) \
              after -= exclude [-1].under; \
        } \
        else \
        { \
            if (value_ > exclude -> value) \
            { \
                if (low + 1 < high) \
                { \
                    low = mid; \
                    continue; \
                } \
            } \
            else \
              before = 0; \
            after -= exclude -> under; \
        } \
        break; \
    } \
}
#endif

#define ENET_CONTEXT_NOT_EXCLUDED(value_, after, before)

size_t
enet_range_coder_decompress (void * context, const enet_uint8 * inData, size_t inLimit, enet_uint8 * outData, size_t outLimit)
{
    ENetRangeCoder * rangeCoder = (ENetRangeCoder *) context;
    enet_uint8 * outStart = outData, * outEnd = & outData [outLimit];
    const enet_uint8 * inEnd = & inData [inLimit];
    enet_uint32 decodeLow = 0, decodeCode = 0, decodeRange = ~0;
    ENetSymbol * root;
    enet_uint16 predicted = 0;
    size_t order = 0, nextSymbol = 0;
#ifdef ENET_CONTEXT_EXCLUSION
    ENetExclude excludes [256];
    ENetExclude * nextExclude = excludes;
#endif
  
    if (rangeCoder == NULL || inLimit <= 0)
      return 0;

    ENET_CONTEXT_CREATE (root, ENET_CONTEXT_ESCAPE_MINIMUM, ENET_CONTEXT_SYMBOL_MINIMUM);

    ENET_RANGE_CODER_SEED;

    for (;;)
    {
        ENetSymbol * subcontext, * symbol, * patch;
#ifdef ENET_CONTEXT_EXCLUSION
        const ENetSymbol * childContext = & emptyContext;
#endif
        enet_uint8 value = 0;
        enet_uint16 code, under, count, bottom, * parent = & predicted, total;

        for (subcontext = & rangeCoder -> symbols [predicted];
             subcontext != root;
#ifdef ENET_CONTEXT_EXCLUSION
             childContext = subcontext, 
#endif
                subcontext = & rangeCoder -> symbols [subcontext -> parent])
        {
            if (subcontext -> escapes <= 0)
              continue;
            total = subcontext -> total;
#ifdef ENET_CONTEXT_EXCLUSION
            if (childContext -> total > 0) 
              ENET_CONTEXT_DECODE_EXCLUDE (childContext, total, 0); 
#endif
            if (subcontext -> escapes >= total)
              continue;
            code = ENET_RANGE_CODER_READ (total);
            if (code < subcontext -> escapes) 
            {
                ENET_RANGE_CODER_DECODE (0, subcontext -> escapes, total); 
                continue;
            }
            code -= subcontext -> escapes;
#ifdef ENET_CONTEXT_EXCLUSION
            if (childContext -> total > 0)
            {
                ENET_CONTEXT_TRY_DECODE (subcontext, symbol, code, value, under, count, ENET_SUBCONTEXT_SYMBOL_DELTA, 0, ENET_CONTEXT_EXCLUDED); 
            }
            else
#endif
            {
                ENET_CONTEXT_TRY_DECODE (subcontext, symbol, code, value, under, count, ENET_SUBCONTEXT_SYMBOL_DELTA, 0, ENET_CONTEXT_NOT_EXCLUDED); 
            }
            bottom = symbol - rangeCoder -> symbols;
            ENET_RANGE_CODER_DECODE (subcontext -> escapes + under, count, total);
            subcontext -> total += ENET_SUBCONTEXT_SYMBOL_DELTA;
            if (count > 0xFF - 2*ENET_SUBCONTEXT_SYMBOL_DELTA || subcontext -> total > ENET_RANGE_CODER_BOTTOM - 0x100)
              ENET_CONTEXT_RESCALE (subcontext, 0);
            goto patchContexts;
        }

        total = root -> total;
#ifdef ENET_CONTEXT_EXCLUSION
        if (childContext -> total > 0)
          ENET_CONTEXT_DECODE_EXCLUDE (childContext, total, ENET_CONTEXT_SYMBOL_MINIMUM);  
#endif
        code = ENET_RANGE_CODER_READ (total);
        if (code < root -> escapes)
        {
            ENET_RANGE_CODER_DECODE (0, root -> escapes, total);
            break;
        }
        code -= root -> escapes;
#ifdef ENET_CONTEXT_EXCLUSION
        if (childContext -> total > 0)
        {
            ENET_CONTEXT_ROOT_DECODE (root, symbol, code, value, under, count, ENET_CONTEXT_SYMBOL_DELTA, ENET_CONTEXT_SYMBOL_MINIMUM, ENET_CONTEXT_EXCLUDED); 
        }
        else
#endif
        {
            ENET_CONTEXT_ROOT_DECODE (root, symbol, code, value, under, count, ENET_CONTEXT_SYMBOL_DELTA, ENET_CONTEXT_SYMBOL_MINIMUM, ENET_CONTEXT_NOT_EXCLUDED); 
        }
        bottom = symbol - rangeCoder -> symbols;
        ENET_RANGE_CODER_DECODE (root -> escapes + under, count, total);
        root -> total += ENET_CONTEXT_SYMBOL_DELTA;
        if (count > 0xFF - 2*ENET_CONTEXT_SYMBOL_DELTA + ENET_CONTEXT_SYMBOL_MINIMUM || root -> total > ENET_RANGE_CODER_BOTTOM - 0x100)
          ENET_CONTEXT_RESCALE (root, ENET_CONTEXT_SYMBOL_MINIMUM);

    patchContexts:
        for (patch = & rangeCoder -> symbols [predicted];
             patch != subcontext;
             patch = & rangeCoder -> symbols [patch -> parent])
        {
            ENET_CONTEXT_ENCODE (patch, symbol, value, under, count, ENET_SUBCONTEXT_SYMBOL_DELTA, 0);
            * parent = symbol - rangeCoder -> symbols;
            parent = & symbol -> parent;
            if (count <= 0)
            {
                patch -> escapes += ENET_SUBCONTEXT_ESCAPE_DELTA;
                patch -> total += ENET_SUBCONTEXT_ESCAPE_DELTA;
            }
            patch -> total += ENET_SUBCONTEXT_SYMBOL_DELTA; 
            if (count > 0xFF - 2*ENET_SUBCONTEXT_SYMBOL_DELTA || patch -> total > ENET_RANGE_CODER_BOTTOM - 0x100)
              ENET_CONTEXT_RESCALE (patch, 0);
        }
        * parent = bottom;

        ENET_RANGE_CODER_OUTPUT (value);

        if (order >= ENET_SUBCONTEXT_ORDER)
          predicted = rangeCoder -> symbols [predicted].parent;
        else
          order ++;
        ENET_RANGE_CODER_FREE_SYMBOLS;
    }
                        
    return (size_t) (outData - outStart);
}

/** Sets the packet compressor the host should use to the default range coder.
    @param host host to enable the range coder for
    @returns 0 on success, < 0 on failure
*/
int
enet_host_compress_with_range_coder (ENetHost * host)
{
    ENetCompressor compressor;
    memset (& compressor, 0, sizeof (compressor));
    compressor.context = enet_range_coder_create();
    if (compressor.context == NULL)
      return -1;
    compressor.compress = enet_range_coder_compress;
    compressor.decompress = enet_range_coder_decompress;
    compressor.destroy = enet_range_coder_destroy;
    enet_host_compress (host, & compressor);
    return 0;
}    
     
// ----- host.c -----

/** Creates a host for communicating to peers.  

    @param address   the address at which other peers may connect to this host.  If NULL, then no peers may connect to the host.
    @param peerCount the maximum number of peers that should be allocated for the host.
    @param channelLimit the maximum number of channels allowed; if 0, then this is equivalent to ENET_PROTOCOL_MAXIMUM_CHANNEL_COUNT
    @param incomingBandwidth downstream bandwidth of the host in bytes/second; if 0, ENet will assume unlimited bandwidth.
    @param outgoingBandwidth upstream bandwidth of the host in bytes/second; if 0, ENet will assume unlimited bandwidth.

    @returns the host on success and NULL on failure

    @remarks ENet will strategically drop packets on specific sides of a connection between hosts
    to ensure the host's bandwidth is not overwhelmed.  The bandwidth parameters also determine
    the window size of a connection which limits the amount of reliable packets that may be in transit
    at any given time.
*/
ENetHost *
enet_host_create (const ENetAddress * address, size_t peerCount, size_t channelLimit, enet_uint32 incomingBandwidth, enet_uint32 outgoingBandwidth)
{
    ENetHost * host;
    ENetPeer * currentPeer;

    if (peerCount > ENET_PROTOCOL_MAXIMUM_PEER_ID)
      return NULL;

    host = (ENetHost *) enet_malloc (sizeof (ENetHost));
    if (host == NULL)
      return NULL;
    memset (host, 0, sizeof (ENetHost));

    host -> peers = (ENetPeer *) enet_malloc (peerCount * sizeof (ENetPeer));
    if (host -> peers == NULL)
    {
       enet_free (host);

       return NULL;
    }
    memset (host -> peers, 0, peerCount * sizeof (ENetPeer));

    host -> socket = enet_socket_create (ENET_SOCKET_TYPE_DATAGRAM);
    if (host -> socket == ENET_SOCKET_NULL || (address != NULL && enet_socket_bind (host -> socket, address) < 0))
    {
       if (host -> socket != ENET_SOCKET_NULL)
         enet_socket_destroy (host -> socket);

       enet_free (host -> peers);
       enet_free (host);

       return NULL;
    }

    enet_socket_set_option (host -> socket, ENET_SOCKOPT_NONBLOCK, 1);
    enet_socket_set_option (host -> socket, ENET_SOCKOPT_BROADCAST, 1);
    enet_socket_set_option (host -> socket, ENET_SOCKOPT_RCVBUF, ENET_HOST_RECEIVE_BUFFER_SIZE);
    enet_socket_set_option (host -> socket, ENET_SOCKOPT_SNDBUF, ENET_HOST_SEND_BUFFER_SIZE);

    if (address != NULL && enet_socket_get_address (host -> socket, & host -> address) < 0)   
      host -> address = * address;

    if (! channelLimit || channelLimit > ENET_PROTOCOL_MAXIMUM_CHANNEL_COUNT)
      channelLimit = ENET_PROTOCOL_MAXIMUM_CHANNEL_COUNT;
    else
    if (channelLimit < ENET_PROTOCOL_MINIMUM_CHANNEL_COUNT)
      channelLimit = ENET_PROTOCOL_MINIMUM_CHANNEL_COUNT;

    host -> randomSeed = (enet_uint32) (size_t) host;
    host -> randomSeed += enet_host_random_seed ();
    host -> randomSeed = (host -> randomSeed << 16) | (host -> randomSeed >> 16);
    host -> channelLimit = channelLimit;
    host -> incomingBandwidth = incomingBandwidth;
    host -> outgoingBandwidth = outgoingBandwidth;
    host -> bandwidthThrottleEpoch = 0;
    host -> recalculateBandwidthLimits = 0;
    host -> mtu = ENET_HOST_DEFAULT_MTU;
    host -> peerCount = peerCount;
    host -> commandCount = 0;
    host -> bufferCount = 0;
    host -> checksum = NULL;
    host -> receivedAddress.host = ENET_HOST_ANY;
    host -> receivedAddress.port = 0;
    host -> receivedData = NULL;
    host -> receivedDataLength = 0;
     
    host -> totalSentData = 0;
    host -> totalSentPackets = 0;
    host -> totalReceivedData = 0;
    host -> totalReceivedPackets = 0;

    host -> connectedPeers = 0;
    host -> bandwidthLimitedPeers = 0;
    host -> duplicatePeers = ENET_PROTOCOL_MAXIMUM_PEER_ID;
    host -> maximumPacketSize = ENET_HOST_DEFAULT_MAXIMUM_PACKET_SIZE;
    host -> maximumWaitingData = ENET_HOST_DEFAULT_MAXIMUM_WAITING_DATA;

    host -> compressor.context = NULL;
    host -> compressor.compress = NULL;
    host -> compressor.decompress = NULL;
    host -> compressor.destroy = NULL;

    host -> intercept = NULL;

    enet_list_clear (& host -> dispatchQueue);

    for (currentPeer = host -> peers;
         currentPeer < & host -> peers [host -> peerCount];
         ++ currentPeer)
    {
       currentPeer -> host = host;
       currentPeer -> incomingPeerID = currentPeer - host -> peers;
       currentPeer -> outgoingSessionID = currentPeer -> incomingSessionID = 0xFF;
       currentPeer -> data = NULL;

       enet_list_clear (& currentPeer -> acknowledgements);
       enet_list_clear (& currentPeer -> sentReliableCommands);
       enet_list_clear (& currentPeer -> sentUnreliableCommands);
       enet_list_clear (& currentPeer -> outgoingCommands);
       enet_list_clear (& currentPeer -> dispatchedCommands);

       enet_peer_reset (currentPeer);
    }

    return host;
}

/** Destroys the host and all resources associated with it.
    @param host pointer to the host to destroy
*/
void
enet_host_destroy (ENetHost * host)
{
    ENetPeer * currentPeer;

    if (host == NULL)
      return;

    enet_socket_destroy (host -> socket);

    for (currentPeer = host -> peers;
         currentPeer < & host -> peers [host -> peerCount];
         ++ currentPeer)
    {
       enet_peer_reset (currentPeer);
    }

    if (host -> compressor.context != NULL && host -> compressor.destroy)
      (* host -> compressor.destroy) (host -> compressor.context);

    enet_free (host -> peers);
    enet_free (host);
}

enet_uint32
enet_host_random (ENetHost * host)
{
    /* Mulberry32 by Tommy Ettinger */
    enet_uint32 n = (host -> randomSeed += 0x6D2B79F5U);
    n = (n ^ (n >> 15)) * (n | 1U);
    n ^= n + (n ^ (n >> 7)) * (n | 61U);
    return n ^ (n >> 14);
}

/** Initiates a connection to a foreign host.
    @param host host seeking the connection
    @param address destination for the connection
    @param channelCount number of channels to allocate
    @param data user data supplied to the receiving host 
    @returns a peer representing the foreign host on success, NULL on failure
    @remarks The peer returned will have not completed the connection until enet_host_service()
    notifies of an ENET_EVENT_TYPE_CONNECT event for the peer.
*/
ENetPeer *
enet_host_connect (ENetHost * host, const ENetAddress * address, size_t channelCount, enet_uint32 data)
{
    ENetPeer * currentPeer;
    ENetChannel * channel;
    ENetProtocol command;

    if (channelCount < ENET_PROTOCOL_MINIMUM_CHANNEL_COUNT)
      channelCount = ENET_PROTOCOL_MINIMUM_CHANNEL_COUNT;
    else
    if (channelCount > ENET_PROTOCOL_MAXIMUM_CHANNEL_COUNT)
      channelCount = ENET_PROTOCOL_MAXIMUM_CHANNEL_COUNT;

    for (currentPeer = host -> peers;
         currentPeer < & host -> peers [host -> peerCount];
         ++ currentPeer)
    {
       if (currentPeer -> state == ENET_PEER_STATE_DISCONNECTED)
         break;
    }

    if (currentPeer >= & host -> peers [host -> peerCount])
      return NULL;

    currentPeer -> channels = (ENetChannel *) enet_malloc (channelCount * sizeof (ENetChannel));
    if (currentPeer -> channels == NULL)
      return NULL;
    currentPeer -> channelCount = channelCount;
    currentPeer -> state = ENET_PEER_STATE_CONNECTING;
    currentPeer -> address = * address;
    currentPeer -> connectID = enet_host_random (host);

    if (host -> outgoingBandwidth == 0)
      currentPeer -> windowSize = ENET_PROTOCOL_MAXIMUM_WINDOW_SIZE;
    else
      currentPeer -> windowSize = (host -> outgoingBandwidth /
                                    ENET_PEER_WINDOW_SIZE_SCALE) * 
                                      ENET_PROTOCOL_MINIMUM_WINDOW_SIZE;

    if (currentPeer -> windowSize < ENET_PROTOCOL_MINIMUM_WINDOW_SIZE)
      currentPeer -> windowSize = ENET_PROTOCOL_MINIMUM_WINDOW_SIZE;
    else
    if (currentPeer -> windowSize > ENET_PROTOCOL_MAXIMUM_WINDOW_SIZE)
      currentPeer -> windowSize = ENET_PROTOCOL_MAXIMUM_WINDOW_SIZE;
         
    for (channel = currentPeer -> channels;
         channel < & currentPeer -> channels [channelCount];
         ++ channel)
    {
        channel -> outgoingReliableSequenceNumber = 0;
        channel -> outgoingUnreliableSequenceNumber = 0;
        channel -> incomingReliableSequenceNumber = 0;
        channel -> incomingUnreliableSequenceNumber = 0;

        enet_list_clear (& channel -> incomingReliableCommands);
        enet_list_clear (& channel -> incomingUnreliableCommands);

        channel -> usedReliableWindows = 0;
        memset (channel -> reliableWindows, 0, sizeof (channel -> reliableWindows));
    }
        
    command.header.command = ENET_PROTOCOL_COMMAND_CONNECT | ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
    command.header.channelID = 0xFF;
    command.connect.outgoingPeerID = ENET_HOST_TO_NET_16 (currentPeer -> incomingPeerID);
    command.connect.incomingSessionID = currentPeer -> incomingSessionID;
    command.connect.outgoingSessionID = currentPeer -> outgoingSessionID;
    command.connect.mtu = ENET_HOST_TO_NET_32 (currentPeer -> mtu);
    command.connect.windowSize = ENET_HOST_TO_NET_32 (currentPeer -> windowSize);
    command.connect.channelCount = ENET_HOST_TO_NET_32 (channelCount);
    command.connect.incomingBandwidth = ENET_HOST_TO_NET_32 (host -> incomingBandwidth);
    command.connect.outgoingBandwidth = ENET_HOST_TO_NET_32 (host -> outgoingBandwidth);
    command.connect.packetThrottleInterval = ENET_HOST_TO_NET_32 (currentPeer -> packetThrottleInterval);
    command.connect.packetThrottleAcceleration = ENET_HOST_TO_NET_32 (currentPeer -> packetThrottleAcceleration);
    command.connect.packetThrottleDeceleration = ENET_HOST_TO_NET_32 (currentPeer -> packetThrottleDeceleration);
    command.connect.connectID = currentPeer -> connectID;
    command.connect.data = ENET_HOST_TO_NET_32 (data);
 
    enet_peer_queue_outgoing_command (currentPeer, & command, NULL, 0, 0);

    return currentPeer;
}

/** Queues a packet to be sent to all peers associated with the host.
    @param host host on which to broadcast the packet
    @param channelID channel on which to broadcast
    @param packet packet to broadcast
*/
void
enet_host_broadcast (ENetHost * host, enet_uint8 channelID, ENetPacket * packet)
{
    ENetPeer * currentPeer;

    for (currentPeer = host -> peers;
         currentPeer < & host -> peers [host -> peerCount];
         ++ currentPeer)
    {
       if (currentPeer -> state != ENET_PEER_STATE_CONNECTED)
         continue;

       enet_peer_send (currentPeer, channelID, packet);
    }

    if (packet -> referenceCount == 0)
      enet_packet_destroy (packet);
}

/** Sets the packet compressor the host should use to compress and decompress packets.
    @param host host to enable or disable compression for
    @param compressor callbacks for for the packet compressor; if NULL, then compression is disabled
*/
void
enet_host_compress (ENetHost * host, const ENetCompressor * compressor)
{
    if (host -> compressor.context != NULL && host -> compressor.destroy)
      (* host -> compressor.destroy) (host -> compressor.context);

    if (compressor)
      host -> compressor = * compressor;
    else
      host -> compressor.context = NULL;
}

/** Limits the maximum allowed channels of future incoming connections.
    @param host host to limit
    @param channelLimit the maximum number of channels allowed; if 0, then this is equivalent to ENET_PROTOCOL_MAXIMUM_CHANNEL_COUNT
*/
void
enet_host_channel_limit (ENetHost * host, size_t channelLimit)
{
    if (! channelLimit || channelLimit > ENET_PROTOCOL_MAXIMUM_CHANNEL_COUNT)
      channelLimit = ENET_PROTOCOL_MAXIMUM_CHANNEL_COUNT;
    else
    if (channelLimit < ENET_PROTOCOL_MINIMUM_CHANNEL_COUNT)
      channelLimit = ENET_PROTOCOL_MINIMUM_CHANNEL_COUNT;

    host -> channelLimit = channelLimit;
}


/** Adjusts the bandwidth limits of a host.
    @param host host to adjust
    @param incomingBandwidth new incoming bandwidth
    @param outgoingBandwidth new outgoing bandwidth
    @remarks the incoming and outgoing bandwidth parameters are identical in function to those
    specified in enet_host_create().
*/
void
enet_host_bandwidth_limit (ENetHost * host, enet_uint32 incomingBandwidth, enet_uint32 outgoingBandwidth)
{
    host -> incomingBandwidth = incomingBandwidth;
    host -> outgoingBandwidth = outgoingBandwidth;
    host -> recalculateBandwidthLimits = 1;
}

void
enet_host_bandwidth_throttle (ENetHost * host)
{
    enet_uint32 timeCurrent = enet_time_get (),
           elapsedTime = timeCurrent - host -> bandwidthThrottleEpoch,
           peersRemaining = (enet_uint32) host -> connectedPeers,
           dataTotal = ~0,
           bandwidth = ~0,
           throttle = 0,
           bandwidthLimit = 0;
    int needsAdjustment = host -> bandwidthLimitedPeers > 0 ? 1 : 0;
    ENetPeer * peer;
    ENetProtocol command;

    if (elapsedTime < ENET_HOST_BANDWIDTH_THROTTLE_INTERVAL)
      return;

    host -> bandwidthThrottleEpoch = timeCurrent;

    if (peersRemaining == 0)
      return;

    if (host -> outgoingBandwidth != 0)
    {
        dataTotal = 0;
        bandwidth = (host -> outgoingBandwidth * elapsedTime) / 1000;

        for (peer = host -> peers;
             peer < & host -> peers [host -> peerCount];
            ++ peer)
        {
            if (peer -> state != ENET_PEER_STATE_CONNECTED && peer -> state != ENET_PEER_STATE_DISCONNECT_LATER)
              continue;

            dataTotal += peer -> outgoingDataTotal;
        }
    }

    while (peersRemaining > 0 && needsAdjustment != 0)
    {
        needsAdjustment = 0;
        
        if (dataTotal <= bandwidth)
          throttle = ENET_PEER_PACKET_THROTTLE_SCALE;
        else
          throttle = (bandwidth * ENET_PEER_PACKET_THROTTLE_SCALE) / dataTotal;

        for (peer = host -> peers;
             peer < & host -> peers [host -> peerCount];
             ++ peer)
        {
            enet_uint32 peerBandwidth;
            
            if ((peer -> state != ENET_PEER_STATE_CONNECTED && peer -> state != ENET_PEER_STATE_DISCONNECT_LATER) ||
                peer -> incomingBandwidth == 0 ||
                peer -> outgoingBandwidthThrottleEpoch == timeCurrent)
              continue;

            peerBandwidth = (peer -> incomingBandwidth * elapsedTime) / 1000;
            if ((throttle * peer -> outgoingDataTotal) / ENET_PEER_PACKET_THROTTLE_SCALE <= peerBandwidth)
              continue;

            peer -> packetThrottleLimit = (peerBandwidth * 
                                            ENET_PEER_PACKET_THROTTLE_SCALE) / peer -> outgoingDataTotal;
            
            if (peer -> packetThrottleLimit == 0)
              peer -> packetThrottleLimit = 1;
            
            if (peer -> packetThrottle > peer -> packetThrottleLimit)
              peer -> packetThrottle = peer -> packetThrottleLimit;

            peer -> outgoingBandwidthThrottleEpoch = timeCurrent;

            peer -> incomingDataTotal = 0;
            peer -> outgoingDataTotal = 0;

            needsAdjustment = 1;
            -- peersRemaining;
            bandwidth -= peerBandwidth;
            dataTotal -= peerBandwidth;
        }
    }

    if (peersRemaining > 0)
    {
        if (dataTotal <= bandwidth)
          throttle = ENET_PEER_PACKET_THROTTLE_SCALE;
        else
          throttle = (bandwidth * ENET_PEER_PACKET_THROTTLE_SCALE) / dataTotal;

        for (peer = host -> peers;
             peer < & host -> peers [host -> peerCount];
             ++ peer)
        {
            if ((peer -> state != ENET_PEER_STATE_CONNECTED && peer -> state != ENET_PEER_STATE_DISCONNECT_LATER) ||
                peer -> outgoingBandwidthThrottleEpoch == timeCurrent)
              continue;

            peer -> packetThrottleLimit = throttle;

            if (peer -> packetThrottle > peer -> packetThrottleLimit)
              peer -> packetThrottle = peer -> packetThrottleLimit;

            peer -> incomingDataTotal = 0;
            peer -> outgoingDataTotal = 0;
        }
    }

    if (host -> recalculateBandwidthLimits)
    {
       host -> recalculateBandwidthLimits = 0;

       peersRemaining = (enet_uint32) host -> connectedPeers;
       bandwidth = host -> incomingBandwidth;
       needsAdjustment = 1;

       if (bandwidth == 0)
         bandwidthLimit = 0;
       else
       while (peersRemaining > 0 && needsAdjustment != 0)
       {
           needsAdjustment = 0;
           bandwidthLimit = bandwidth / peersRemaining;

           for (peer = host -> peers;
                peer < & host -> peers [host -> peerCount];
                ++ peer)
           {
               if ((peer -> state != ENET_PEER_STATE_CONNECTED && peer -> state != ENET_PEER_STATE_DISCONNECT_LATER) ||
                   peer -> incomingBandwidthThrottleEpoch == timeCurrent)
                 continue;

               if (peer -> outgoingBandwidth > 0 &&
                   peer -> outgoingBandwidth >= bandwidthLimit)
                 continue;

               peer -> incomingBandwidthThrottleEpoch = timeCurrent;
 
               needsAdjustment = 1;
               -- peersRemaining;
               bandwidth -= peer -> outgoingBandwidth;
           }
       }

       for (peer = host -> peers;
            peer < & host -> peers [host -> peerCount];
            ++ peer)
       {
           if (peer -> state != ENET_PEER_STATE_CONNECTED && peer -> state != ENET_PEER_STATE_DISCONNECT_LATER)
             continue;

           command.header.command = ENET_PROTOCOL_COMMAND_BANDWIDTH_LIMIT | ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
           command.header.channelID = 0xFF;
           command.bandwidthLimit.outgoingBandwidth = ENET_HOST_TO_NET_32 (host -> outgoingBandwidth);

           if (peer -> incomingBandwidthThrottleEpoch == timeCurrent)
             command.bandwidthLimit.incomingBandwidth = ENET_HOST_TO_NET_32 (peer -> outgoingBandwidth);
           else
             command.bandwidthLimit.incomingBandwidth = ENET_HOST_TO_NET_32 (bandwidthLimit);

           enet_peer_queue_outgoing_command (peer, & command, NULL, 0, 0);
       } 
    }
}

// ----- list.c -----

void
enet_list_clear (ENetList * list)
{
   list -> sentinel.next = & list -> sentinel;
   list -> sentinel.previous = & list -> sentinel;
}

ENetListIterator
enet_list_insert (ENetListIterator position, void * data)
{
   ENetListIterator result = (ENetListIterator) data;

   result -> previous = position -> previous;
   result -> next = position;

   result -> previous -> next = result;
   position -> previous = result;

   return result;
}

void *
enet_list_remove (ENetListIterator position)
{
   position -> previous -> next = position -> next;
   position -> next -> previous = position -> previous;

   return position;
}

ENetListIterator
enet_list_move (ENetListIterator position, void * dataFirst, void * dataLast)
{
   ENetListIterator first = (ENetListIterator) dataFirst,
                    last = (ENetListIterator) dataLast;

   first -> previous -> next = last -> next;
   last -> next -> previous = first -> previous;

   first -> previous = position -> previous;
   last -> next = position;

   first -> previous -> next = first;
   position -> previous = last;
    
   return first;
}

size_t
enet_list_size (ENetList * list)
{
   size_t size = 0;
   ENetListIterator position;

   for (position = enet_list_begin (list);
        position != enet_list_end (list);
        position = enet_list_next (position))
     ++ size;
   
   return size;
}

// ----- packet.c -----

/** Creates a packet that may be sent to a peer.
    @param data         initial contents of the packet's data; the packet's data will remain uninitialized if data is NULL.
    @param dataLength   size of the data allocated for this packet
    @param flags        flags for this packet as described for the ENetPacket structure.
    @returns the packet on success, NULL on failure
*/
ENetPacket *
enet_packet_create (const void * data, size_t dataLength, enet_uint32 flags)
{
    ENetPacket * packet = (ENetPacket *) enet_malloc (sizeof (ENetPacket));
    if (packet == NULL)
      return NULL;

    if (flags & ENET_PACKET_FLAG_NO_ALLOCATE)
      packet -> data = (enet_uint8 *) data;
    else
    if (dataLength <= 0)
      packet -> data = NULL;
    else
    {
       packet -> data = (enet_uint8 *) enet_malloc (dataLength);
       if (packet -> data == NULL)
       {
          enet_free (packet);
          return NULL;
       }

       if (data != NULL)
         memcpy (packet -> data, data, dataLength);
    }

    packet -> referenceCount = 0;
    packet -> flags = flags;
    packet -> dataLength = dataLength;
    packet -> freeCallback = NULL;
    packet -> userData = NULL;

    return packet;
}

/** Destroys the packet and deallocates its data.
    @param packet packet to be destroyed
*/
void
enet_packet_destroy (ENetPacket * packet)
{
    if (packet == NULL)
      return;

    if (packet -> freeCallback != NULL)
      (* packet -> freeCallback) (packet);
    if (! (packet -> flags & ENET_PACKET_FLAG_NO_ALLOCATE) &&
        packet -> data != NULL)
      enet_free (packet -> data);
    enet_free (packet);
}

/** Attempts to resize the data in the packet to length specified in the 
    dataLength parameter 
    @param packet packet to resize
    @param dataLength new size for the packet data
    @returns 0 on success, < 0 on failure
*/
int
enet_packet_resize (ENetPacket * packet, size_t dataLength)
{
    enet_uint8 * newData;
   
    if (dataLength <= packet -> dataLength || (packet -> flags & ENET_PACKET_FLAG_NO_ALLOCATE))
    {
       packet -> dataLength = dataLength;

       return 0;
    }

    newData = (enet_uint8 *) enet_malloc (dataLength);
    if (newData == NULL)
      return -1;

    memcpy (newData, packet -> data, packet -> dataLength);
    enet_free (packet -> data);
    
    packet -> data = newData;
    packet -> dataLength = dataLength;

    return 0;
}

static const enet_uint32 crcTable [256] =
{
    0,          0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3,
    0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988, 0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91,
    0x1DB71064, 0x6AB020F2, 0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
    0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5,
    0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172, 0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B,
    0x35B5A8FA, 0x42B2986C, 0xDBBBC9D6, 0xACBCF940, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
    0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423, 0xCFBA9599, 0xB8BDA50F,
    0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924, 0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D,
    0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
    0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D, 0x91646C97, 0xE6635C01,
    0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E, 0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457,
    0x65B0D9C6, 0x12B7E950, 0x8BBEB8EA, 0xFCB9887C, 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
    0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2, 0x4ADFA541, 0x3DD895D7, 0xA4D1C46D, 0xD3D6F4FB,
    0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0, 0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9,
    0x5005713C, 0x270241AA, 0xBE0B1010, 0xC90C2086, 0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
    0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81, 0xB7BD5C3B, 0xC0BA6CAD,
    0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A, 0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683,
    0xE3630B12, 0x94643B84, 0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
    0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D, 0x806567CB, 0x196C3671, 0x6E6B06E7,
    0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC, 0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5,
    0xD6D6A3E8, 0xA1D1937E, 0x38D8C2C4, 0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
    0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60, 0xDF60EFC3, 0xA867DF55, 0x316E8EEF, 0x4669BE79,
    0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236, 0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F,
    0xC5BA3BBE, 0xB2BD0B28, 0x2BB45A92, 0x5CB36A04, 0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
    0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F, 0x72076785, 0x5005713,
    0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38, 0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0xBDBDF21,
    0x86D3D2D4, 0xF1D4E242, 0x68DDB3F8, 0x1FDA836E, 0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
    0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C, 0x8F659EFF, 0xF862AE69, 0x616BFFD3, 0x166CCF45,
    0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2, 0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB,
    0xAED16A4A, 0xD9D65ADC, 0x40DF0B66, 0x37D83BF0, 0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
    0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6, 0xBAD03605, 0xCDD70693, 0x54DE5729, 0x23D967BF,
    0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94, 0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D
};

enet_uint32
enet_crc32 (const ENetBuffer * buffers, size_t bufferCount)
{
    enet_uint32 crc = 0xFFFFFFFF;

    while (bufferCount -- > 0)
    {
        const enet_uint8 * data = (const enet_uint8 *) buffers -> data,
                         * dataEnd = & data [buffers -> dataLength];

        while (data < dataEnd)
        {
            crc = (crc >> 8) ^ crcTable [(crc & 0xFF) ^ *data++];
        }

        ++ buffers;
    }

    return ENET_HOST_TO_NET_32 (~ crc);
}

// ----- peer.c -----

/** Configures throttle parameter for a peer.

    Unreliable packets are dropped by ENet in response to the varying conditions
    of the Internet connection to the peer.  The throttle represents a probability
    that an unreliable packet should not be dropped and thus sent by ENet to the peer.
    The lowest mean round trip time from the sending of a reliable packet to the
    receipt of its acknowledgement is measured over an amount of time specified by
    the interval parameter in milliseconds.  If a measured round trip time happens to
    be significantly less than the mean round trip time measured over the interval, 
    then the throttle probability is increased to allow more traffic by an amount
    specified in the acceleration parameter, which is a ratio to the ENET_PEER_PACKET_THROTTLE_SCALE
    constant.  If a measured round trip time happens to be significantly greater than
    the mean round trip time measured over the interval, then the throttle probability
    is decreased to limit traffic by an amount specified in the deceleration parameter, which
    is a ratio to the ENET_PEER_PACKET_THROTTLE_SCALE constant.  When the throttle has
    a value of ENET_PEER_PACKET_THROTTLE_SCALE, no unreliable packets are dropped by 
    ENet, and so 100% of all unreliable packets will be sent.  When the throttle has a
    value of 0, all unreliable packets are dropped by ENet, and so 0% of all unreliable
    packets will be sent.  Intermediate values for the throttle represent intermediate
    probabilities between 0% and 100% of unreliable packets being sent.  The bandwidth
    limits of the local and foreign hosts are taken into account to determine a 
    sensible limit for the throttle probability above which it should not raise even in
    the best of conditions.

    @param peer peer to configure 
    @param interval interval, in milliseconds, over which to measure lowest mean RTT; the default value is ENET_PEER_PACKET_THROTTLE_INTERVAL.
    @param acceleration rate at which to increase the throttle probability as mean RTT declines
    @param deceleration rate at which to decrease the throttle probability as mean RTT increases
*/
void
enet_peer_throttle_configure (ENetPeer * peer, enet_uint32 interval, enet_uint32 acceleration, enet_uint32 deceleration)
{
    ENetProtocol command;

    peer -> packetThrottleInterval = interval;
    peer -> packetThrottleAcceleration = acceleration;
    peer -> packetThrottleDeceleration = deceleration;

    command.header.command = ENET_PROTOCOL_COMMAND_THROTTLE_CONFIGURE | ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
    command.header.channelID = 0xFF;

    command.throttleConfigure.packetThrottleInterval = ENET_HOST_TO_NET_32 (interval);
    command.throttleConfigure.packetThrottleAcceleration = ENET_HOST_TO_NET_32 (acceleration);
    command.throttleConfigure.packetThrottleDeceleration = ENET_HOST_TO_NET_32 (deceleration);

    enet_peer_queue_outgoing_command (peer, & command, NULL, 0, 0);
}

int
enet_peer_throttle (ENetPeer * peer, enet_uint32 rtt)
{
    if (peer -> lastRoundTripTime <= peer -> lastRoundTripTimeVariance)
    {
        peer -> packetThrottle = peer -> packetThrottleLimit;
    }
    else
    if (rtt <= peer -> lastRoundTripTime)
    {
        peer -> packetThrottle += peer -> packetThrottleAcceleration;

        if (peer -> packetThrottle > peer -> packetThrottleLimit)
          peer -> packetThrottle = peer -> packetThrottleLimit;

        return 1;
    }
    else
    if (rtt > peer -> lastRoundTripTime + 2 * peer -> lastRoundTripTimeVariance)
    {
        if (peer -> packetThrottle > peer -> packetThrottleDeceleration)
          peer -> packetThrottle -= peer -> packetThrottleDeceleration;
        else
          peer -> packetThrottle = 0;

        return -1;
    }

    return 0;
}

/** Queues a packet to be sent.

    On success, ENet will assume ownership of the packet, and so enet_packet_destroy
    should not be called on it thereafter. On failure, the caller still must destroy
    the packet on its own as ENet has not queued the packet. The caller can also
    check the packet's referenceCount field after sending to check if ENet queued
    the packet and thus incremented the referenceCount.

    @param peer destination for the packet
    @param channelID channel on which to send
    @param packet packet to send
    @retval 0 on success
    @retval < 0 on failure
*/
int
enet_peer_send (ENetPeer * peer, enet_uint8 channelID, ENetPacket * packet)
{
   ENetChannel * channel;
   ENetProtocol command;
   size_t fragmentLength;

   if (peer -> state != ENET_PEER_STATE_CONNECTED ||
       channelID >= peer -> channelCount ||
       packet -> dataLength > peer -> host -> maximumPacketSize)
     return -1;

   channel = & peer -> channels [channelID];
   fragmentLength = peer -> mtu - sizeof (ENetProtocolHeader) - sizeof (ENetProtocolSendFragment);
   if (peer -> host -> checksum != NULL)
     fragmentLength -= sizeof(enet_uint32);

   if (packet -> dataLength > fragmentLength)
   {
      enet_uint32 fragmentCount = (packet -> dataLength + fragmentLength - 1) / fragmentLength,
             fragmentNumber,
             fragmentOffset;
      enet_uint8 commandNumber;
      enet_uint16 startSequenceNumber; 
      ENetList fragments;
      ENetOutgoingCommand * fragment;

      if (fragmentCount > ENET_PROTOCOL_MAXIMUM_FRAGMENT_COUNT)
        return -1;

      if ((packet -> flags & (ENET_PACKET_FLAG_RELIABLE | ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT)) == ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT &&
          channel -> outgoingUnreliableSequenceNumber < 0xFFFF)
      {
         commandNumber = ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT;
         startSequenceNumber = ENET_HOST_TO_NET_16 (channel -> outgoingUnreliableSequenceNumber + 1);
      }
      else
      {
         commandNumber = ENET_PROTOCOL_COMMAND_SEND_FRAGMENT | ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
         startSequenceNumber = ENET_HOST_TO_NET_16 (channel -> outgoingReliableSequenceNumber + 1);
      }
        
      enet_list_clear (& fragments);

      for (fragmentNumber = 0,
             fragmentOffset = 0;
           fragmentOffset < packet -> dataLength;
           ++ fragmentNumber,
             fragmentOffset += fragmentLength)
      {
         if (packet -> dataLength - fragmentOffset < fragmentLength)
           fragmentLength = packet -> dataLength - fragmentOffset;

         fragment = (ENetOutgoingCommand *) enet_malloc (sizeof (ENetOutgoingCommand));
         if (fragment == NULL)
         {
            while (! enet_list_empty (& fragments))
            {
               fragment = (ENetOutgoingCommand *) enet_list_remove (enet_list_begin (& fragments));
               
               enet_free (fragment);
            }
            
            return -1;
         }
         
         fragment -> fragmentOffset = fragmentOffset;
         fragment -> fragmentLength = fragmentLength;
         fragment -> packet = packet;
         fragment -> command.header.command = commandNumber;
         fragment -> command.header.channelID = channelID;
         fragment -> command.sendFragment.startSequenceNumber = startSequenceNumber;
         fragment -> command.sendFragment.dataLength = ENET_HOST_TO_NET_16 (fragmentLength);
         fragment -> command.sendFragment.fragmentCount = ENET_HOST_TO_NET_32 (fragmentCount);
         fragment -> command.sendFragment.fragmentNumber = ENET_HOST_TO_NET_32 (fragmentNumber);
         fragment -> command.sendFragment.totalLength = ENET_HOST_TO_NET_32 (packet -> dataLength);
         fragment -> command.sendFragment.fragmentOffset = ENET_NET_TO_HOST_32 (fragmentOffset);
        
         enet_list_insert (enet_list_end (& fragments), fragment);
      }

      packet -> referenceCount += fragmentNumber;

      while (! enet_list_empty (& fragments))
      {
         fragment = (ENetOutgoingCommand *) enet_list_remove (enet_list_begin (& fragments));
 
         enet_peer_setup_outgoing_command (peer, fragment);
      }

      return 0;
   }

   command.header.channelID = channelID;

   if ((packet -> flags & (ENET_PACKET_FLAG_RELIABLE | ENET_PACKET_FLAG_UNSEQUENCED)) == ENET_PACKET_FLAG_UNSEQUENCED)
   {
      command.header.command = ENET_PROTOCOL_COMMAND_SEND_UNSEQUENCED | ENET_PROTOCOL_COMMAND_FLAG_UNSEQUENCED;
      command.sendUnsequenced.dataLength = ENET_HOST_TO_NET_16 (packet -> dataLength);
   }
   else 
   if (packet -> flags & ENET_PACKET_FLAG_RELIABLE || channel -> outgoingUnreliableSequenceNumber >= 0xFFFF)
   {
      command.header.command = ENET_PROTOCOL_COMMAND_SEND_RELIABLE | ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
      command.sendReliable.dataLength = ENET_HOST_TO_NET_16 (packet -> dataLength);
   }
   else
   {
      command.header.command = ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE;
      command.sendUnreliable.dataLength = ENET_HOST_TO_NET_16 (packet -> dataLength);
   }

   if (enet_peer_queue_outgoing_command (peer, & command, packet, 0, packet -> dataLength) == NULL)
     return -1;

   return 0;
}

/** Attempts to dequeue any incoming queued packet.
    @param peer peer to dequeue packets from
    @param channelID holds the channel ID of the channel the packet was received on success
    @returns a pointer to the packet, or NULL if there are no available incoming queued packets
*/
ENetPacket *
enet_peer_receive (ENetPeer * peer, enet_uint8 * channelID)
{
   ENetIncomingCommand * incomingCommand;
   ENetPacket * packet;
   
   if (enet_list_empty (& peer -> dispatchedCommands))
     return NULL;

   incomingCommand = (ENetIncomingCommand *) enet_list_remove (enet_list_begin (& peer -> dispatchedCommands));

   if (channelID != NULL)
     * channelID = incomingCommand -> command.header.channelID;

   packet = incomingCommand -> packet;

   -- packet -> referenceCount;

   if (incomingCommand -> fragments != NULL)
     enet_free (incomingCommand -> fragments);

   enet_free (incomingCommand);

   peer -> totalWaitingData -= packet -> dataLength;

   return packet;
}

static void
enet_peer_reset_outgoing_commands (ENetList * queue)
{
    ENetOutgoingCommand * outgoingCommand;

    while (! enet_list_empty (queue))
    {
       outgoingCommand = (ENetOutgoingCommand *) enet_list_remove (enet_list_begin (queue));

       if (outgoingCommand -> packet != NULL)
       {
          -- outgoingCommand -> packet -> referenceCount;

          if (outgoingCommand -> packet -> referenceCount == 0)
            enet_packet_destroy (outgoingCommand -> packet);
       }

       enet_free (outgoingCommand);
    }
}

static void
enet_peer_remove_incoming_commands (ENetList * queue, ENetListIterator startCommand, ENetListIterator endCommand, ENetIncomingCommand * excludeCommand)
{
    ENetListIterator currentCommand;    
    
    for (currentCommand = startCommand; currentCommand != endCommand; )
    {
       ENetIncomingCommand * incomingCommand = (ENetIncomingCommand *) currentCommand;

       currentCommand = enet_list_next (currentCommand);

       if (incomingCommand == excludeCommand)
         continue;

       enet_list_remove (& incomingCommand -> incomingCommandList);
 
       if (incomingCommand -> packet != NULL)
       {
          -- incomingCommand -> packet -> referenceCount;

          if (incomingCommand -> packet -> referenceCount == 0)
            enet_packet_destroy (incomingCommand -> packet);
       }

       if (incomingCommand -> fragments != NULL)
         enet_free (incomingCommand -> fragments);

       enet_free (incomingCommand);
    }
}

static void
enet_peer_reset_incoming_commands (ENetList * queue)
{
    enet_peer_remove_incoming_commands(queue, enet_list_begin (queue), enet_list_end (queue), NULL);
}
 
void
enet_peer_reset_queues (ENetPeer * peer)
{
    ENetChannel * channel;

    if (peer -> flags & ENET_PEER_FLAG_NEEDS_DISPATCH)
    {
       enet_list_remove (& peer -> dispatchList);

       peer -> flags &= ~ ENET_PEER_FLAG_NEEDS_DISPATCH;
    }

    while (! enet_list_empty (& peer -> acknowledgements))
      enet_free (enet_list_remove (enet_list_begin (& peer -> acknowledgements)));

    enet_peer_reset_outgoing_commands (& peer -> sentReliableCommands);
    enet_peer_reset_outgoing_commands (& peer -> sentUnreliableCommands);
    enet_peer_reset_outgoing_commands (& peer -> outgoingCommands);
    enet_peer_reset_incoming_commands (& peer -> dispatchedCommands);

    if (peer -> channels != NULL && peer -> channelCount > 0)
    {
        for (channel = peer -> channels;
             channel < & peer -> channels [peer -> channelCount];
             ++ channel)
        {
            enet_peer_reset_incoming_commands (& channel -> incomingReliableCommands);
            enet_peer_reset_incoming_commands (& channel -> incomingUnreliableCommands);
        }

        enet_free (peer -> channels);
    }

    peer -> channels = NULL;
    peer -> channelCount = 0;
}

void
enet_peer_on_connect (ENetPeer * peer)
{
    if (peer -> state != ENET_PEER_STATE_CONNECTED && peer -> state != ENET_PEER_STATE_DISCONNECT_LATER)
    {
        if (peer -> incomingBandwidth != 0)
          ++ peer -> host -> bandwidthLimitedPeers;

        ++ peer -> host -> connectedPeers;
    }
}

void
enet_peer_on_disconnect (ENetPeer * peer)
{
    if (peer -> state == ENET_PEER_STATE_CONNECTED || peer -> state == ENET_PEER_STATE_DISCONNECT_LATER)
    {
        if (peer -> incomingBandwidth != 0)
          -- peer -> host -> bandwidthLimitedPeers;

        -- peer -> host -> connectedPeers;
    }
}

/** Forcefully disconnects a peer.
    @param peer peer to forcefully disconnect
    @remarks The foreign host represented by the peer is not notified of the disconnection and will timeout
    on its connection to the local host.
*/
void
enet_peer_reset (ENetPeer * peer)
{
    enet_peer_on_disconnect (peer);
        
    peer -> outgoingPeerID = ENET_PROTOCOL_MAXIMUM_PEER_ID;
    peer -> connectID = 0;

    peer -> state = ENET_PEER_STATE_DISCONNECTED;

    peer -> incomingBandwidth = 0;
    peer -> outgoingBandwidth = 0;
    peer -> incomingBandwidthThrottleEpoch = 0;
    peer -> outgoingBandwidthThrottleEpoch = 0;
    peer -> incomingDataTotal = 0;
    peer -> outgoingDataTotal = 0;
    peer -> lastSendTime = 0;
    peer -> lastReceiveTime = 0;
    peer -> nextTimeout = 0;
    peer -> earliestTimeout = 0;
    peer -> packetLossEpoch = 0;
    peer -> packetsSent = 0;
    peer -> packetsLost = 0;
    peer -> packetLoss = 0;
    peer -> packetLossVariance = 0;
    peer -> packetThrottle = ENET_PEER_DEFAULT_PACKET_THROTTLE;
    peer -> packetThrottleLimit = ENET_PEER_PACKET_THROTTLE_SCALE;
    peer -> packetThrottleCounter = 0;
    peer -> packetThrottleEpoch = 0;
    peer -> packetThrottleAcceleration = ENET_PEER_PACKET_THROTTLE_ACCELERATION;
    peer -> packetThrottleDeceleration = ENET_PEER_PACKET_THROTTLE_DECELERATION;
    peer -> packetThrottleInterval = ENET_PEER_PACKET_THROTTLE_INTERVAL;
    peer -> pingInterval = ENET_PEER_PING_INTERVAL;
    peer -> timeoutLimit = ENET_PEER_TIMEOUT_LIMIT;
    peer -> timeoutMinimum = ENET_PEER_TIMEOUT_MINIMUM;
    peer -> timeoutMaximum = ENET_PEER_TIMEOUT_MAXIMUM;
    peer -> lastRoundTripTime = ENET_PEER_DEFAULT_ROUND_TRIP_TIME;
    peer -> lowestRoundTripTime = ENET_PEER_DEFAULT_ROUND_TRIP_TIME;
    peer -> lastRoundTripTimeVariance = 0;
    peer -> highestRoundTripTimeVariance = 0;
    peer -> roundTripTime = ENET_PEER_DEFAULT_ROUND_TRIP_TIME;
    peer -> roundTripTimeVariance = 0;
    peer -> mtu = peer -> host -> mtu;
    peer -> reliableDataInTransit = 0;
    peer -> outgoingReliableSequenceNumber = 0;
    peer -> windowSize = ENET_PROTOCOL_MAXIMUM_WINDOW_SIZE;
    peer -> incomingUnsequencedGroup = 0;
    peer -> outgoingUnsequencedGroup = 0;
    peer -> eventData = 0;
    peer -> totalWaitingData = 0;
    peer -> flags = 0;

    memset (peer -> unsequencedWindow, 0, sizeof (peer -> unsequencedWindow));
    
    enet_peer_reset_queues (peer);
}

/** Sends a ping request to a peer.
    @param peer destination for the ping request
    @remarks ping requests factor into the mean round trip time as designated by the 
    roundTripTime field in the ENetPeer structure.  ENet automatically pings all connected
    peers at regular intervals, however, this function may be called to ensure more
    frequent ping requests.
*/
void
enet_peer_ping (ENetPeer * peer)
{
    ENetProtocol command;

    if (peer -> state != ENET_PEER_STATE_CONNECTED)
      return;

    command.header.command = ENET_PROTOCOL_COMMAND_PING | ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
    command.header.channelID = 0xFF;
   
    enet_peer_queue_outgoing_command (peer, & command, NULL, 0, 0);
}

/** Sets the interval at which pings will be sent to a peer. 
    
    Pings are used both to monitor the liveness of the connection and also to dynamically
    adjust the throttle during periods of low traffic so that the throttle has reasonable
    responsiveness during traffic spikes.

    @param peer the peer to adjust
    @param pingInterval the interval at which to send pings; defaults to ENET_PEER_PING_INTERVAL if 0
*/
void
enet_peer_ping_interval (ENetPeer * peer, enet_uint32 pingInterval)
{
    peer -> pingInterval = pingInterval ? pingInterval : ENET_PEER_PING_INTERVAL;
}

/** Sets the timeout parameters for a peer.

    The timeout parameter control how and when a peer will timeout from a failure to acknowledge
    reliable traffic. Timeout values use an exponential backoff mechanism, where if a reliable
    packet is not acknowledge within some multiple of the average RTT plus a variance tolerance, 
    the timeout will be doubled until it reaches a set limit. If the timeout is thus at this
    limit and reliable packets have been sent but not acknowledged within a certain minimum time 
    period, the peer will be disconnected. Alternatively, if reliable packets have been sent
    but not acknowledged for a certain maximum time period, the peer will be disconnected regardless
    of the current timeout limit value.
    
    @param peer the peer to adjust
    @param timeoutLimit the timeout limit; defaults to ENET_PEER_TIMEOUT_LIMIT if 0
    @param timeoutMinimum the timeout minimum; defaults to ENET_PEER_TIMEOUT_MINIMUM if 0
    @param timeoutMaximum the timeout maximum; defaults to ENET_PEER_TIMEOUT_MAXIMUM if 0
*/

void
enet_peer_timeout (ENetPeer * peer, enet_uint32 timeoutLimit, enet_uint32 timeoutMinimum, enet_uint32 timeoutMaximum)
{
    peer -> timeoutLimit = timeoutLimit ? timeoutLimit : ENET_PEER_TIMEOUT_LIMIT;
    peer -> timeoutMinimum = timeoutMinimum ? timeoutMinimum : ENET_PEER_TIMEOUT_MINIMUM;
    peer -> timeoutMaximum = timeoutMaximum ? timeoutMaximum : ENET_PEER_TIMEOUT_MAXIMUM;
}

/** Force an immediate disconnection from a peer.
    @param peer peer to disconnect
    @param data data describing the disconnection
    @remarks No ENET_EVENT_DISCONNECT event will be generated. The foreign peer is not
    guaranteed to receive the disconnect notification, and is reset immediately upon
    return from this function.
*/
void
enet_peer_disconnect_now (ENetPeer * peer, enet_uint32 data)
{
    ENetProtocol command;

    if (peer -> state == ENET_PEER_STATE_DISCONNECTED)
      return;

    if (peer -> state != ENET_PEER_STATE_ZOMBIE &&
        peer -> state != ENET_PEER_STATE_DISCONNECTING)
    {
        enet_peer_reset_queues (peer);

        command.header.command = ENET_PROTOCOL_COMMAND_DISCONNECT | ENET_PROTOCOL_COMMAND_FLAG_UNSEQUENCED;
        command.header.channelID = 0xFF;
        command.disconnect.data = ENET_HOST_TO_NET_32 (data);

        enet_peer_queue_outgoing_command (peer, & command, NULL, 0, 0);

        enet_host_flush (peer -> host);
    }

    enet_peer_reset (peer);
}

/** Request a disconnection from a peer.
    @param peer peer to request a disconnection
    @param data data describing the disconnection
    @remarks An ENET_EVENT_DISCONNECT event will be generated by enet_host_service()
    once the disconnection is complete.
*/
void
enet_peer_disconnect (ENetPeer * peer, enet_uint32 data)
{
    ENetProtocol command;

    if (peer -> state == ENET_PEER_STATE_DISCONNECTING ||
        peer -> state == ENET_PEER_STATE_DISCONNECTED ||
        peer -> state == ENET_PEER_STATE_ACKNOWLEDGING_DISCONNECT ||
        peer -> state == ENET_PEER_STATE_ZOMBIE)
      return;

    enet_peer_reset_queues (peer);

    command.header.command = ENET_PROTOCOL_COMMAND_DISCONNECT;
    command.header.channelID = 0xFF;
    command.disconnect.data = ENET_HOST_TO_NET_32 (data);

    if (peer -> state == ENET_PEER_STATE_CONNECTED || peer -> state == ENET_PEER_STATE_DISCONNECT_LATER)
      command.header.command |= ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
    else
      command.header.command |= ENET_PROTOCOL_COMMAND_FLAG_UNSEQUENCED;      
    
    enet_peer_queue_outgoing_command (peer, & command, NULL, 0, 0);

    if (peer -> state == ENET_PEER_STATE_CONNECTED || peer -> state == ENET_PEER_STATE_DISCONNECT_LATER)
    {
        enet_peer_on_disconnect (peer);

        peer -> state = ENET_PEER_STATE_DISCONNECTING;
    }
    else
    {
        enet_host_flush (peer -> host);
        enet_peer_reset (peer);
    }
}

/** Request a disconnection from a peer, but only after all queued outgoing packets are sent.
    @param peer peer to request a disconnection
    @param data data describing the disconnection
    @remarks An ENET_EVENT_DISCONNECT event will be generated by enet_host_service()
    once the disconnection is complete.
*/
void
enet_peer_disconnect_later (ENetPeer * peer, enet_uint32 data)
{   
    if ((peer -> state == ENET_PEER_STATE_CONNECTED || peer -> state == ENET_PEER_STATE_DISCONNECT_LATER) && 
        ! (enet_list_empty (& peer -> outgoingCommands) &&
           enet_list_empty (& peer -> sentReliableCommands)))
    {
        peer -> state = ENET_PEER_STATE_DISCONNECT_LATER;
        peer -> eventData = data;
    }
    else
      enet_peer_disconnect (peer, data);
}

ENetAcknowledgement *
enet_peer_queue_acknowledgement (ENetPeer * peer, const ENetProtocol * command, enet_uint16 sentTime)
{
    ENetAcknowledgement * acknowledgement;

    if (command -> header.channelID < peer -> channelCount)
    {
        ENetChannel * channel = & peer -> channels [command -> header.channelID];
        enet_uint16 reliableWindow = command -> header.reliableSequenceNumber / ENET_PEER_RELIABLE_WINDOW_SIZE,
                    currentWindow = channel -> incomingReliableSequenceNumber / ENET_PEER_RELIABLE_WINDOW_SIZE;

        if (command -> header.reliableSequenceNumber < channel -> incomingReliableSequenceNumber)
           reliableWindow += ENET_PEER_RELIABLE_WINDOWS;

        if (reliableWindow >= currentWindow + ENET_PEER_FREE_RELIABLE_WINDOWS - 1 && reliableWindow <= currentWindow + ENET_PEER_FREE_RELIABLE_WINDOWS)
          return NULL;
    }

    acknowledgement = (ENetAcknowledgement *) enet_malloc (sizeof (ENetAcknowledgement));
    if (acknowledgement == NULL)
      return NULL;

    peer -> outgoingDataTotal += sizeof (ENetProtocolAcknowledge);

    acknowledgement -> sentTime = sentTime;
    acknowledgement -> command = * command;
    
    enet_list_insert (enet_list_end (& peer -> acknowledgements), acknowledgement);
    
    return acknowledgement;
}

void
enet_peer_setup_outgoing_command (ENetPeer * peer, ENetOutgoingCommand * outgoingCommand)
{
    peer -> outgoingDataTotal += enet_protocol_command_size (outgoingCommand -> command.header.command) + outgoingCommand -> fragmentLength;

    if (outgoingCommand -> command.header.channelID == 0xFF)
    {
       ++ peer -> outgoingReliableSequenceNumber;

       outgoingCommand -> reliableSequenceNumber = peer -> outgoingReliableSequenceNumber;
       outgoingCommand -> unreliableSequenceNumber = 0;
    }
    else
    {
        ENetChannel * channel = & peer -> channels [outgoingCommand -> command.header.channelID];

        if (outgoingCommand -> command.header.command & ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE)
        {
           ++ channel -> outgoingReliableSequenceNumber;
           channel -> outgoingUnreliableSequenceNumber = 0;

           outgoingCommand -> reliableSequenceNumber = channel -> outgoingReliableSequenceNumber;
           outgoingCommand -> unreliableSequenceNumber = 0;
        }
        else
        if (outgoingCommand -> command.header.command & ENET_PROTOCOL_COMMAND_FLAG_UNSEQUENCED)
        {
           ++ peer -> outgoingUnsequencedGroup;

           outgoingCommand -> reliableSequenceNumber = 0;
           outgoingCommand -> unreliableSequenceNumber = 0;
        }
        else
        {
           if (outgoingCommand -> fragmentOffset == 0)
             ++ channel -> outgoingUnreliableSequenceNumber;

           outgoingCommand -> reliableSequenceNumber = channel -> outgoingReliableSequenceNumber;
           outgoingCommand -> unreliableSequenceNumber = channel -> outgoingUnreliableSequenceNumber;
        }
    }

    outgoingCommand -> sendAttempts = 0;
    outgoingCommand -> sentTime = 0;
    outgoingCommand -> roundTripTimeout = 0;
    outgoingCommand -> roundTripTimeoutLimit = 0;
    outgoingCommand -> command.header.reliableSequenceNumber = ENET_HOST_TO_NET_16 (outgoingCommand -> reliableSequenceNumber);

    switch (outgoingCommand -> command.header.command & ENET_PROTOCOL_COMMAND_MASK)
    {
    case ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE:
        outgoingCommand -> command.sendUnreliable.unreliableSequenceNumber = ENET_HOST_TO_NET_16 (outgoingCommand -> unreliableSequenceNumber);
        break;

    case ENET_PROTOCOL_COMMAND_SEND_UNSEQUENCED:
        outgoingCommand -> command.sendUnsequenced.unsequencedGroup = ENET_HOST_TO_NET_16 (peer -> outgoingUnsequencedGroup);
        break;
    
    default:
        break;
    }

    enet_list_insert (enet_list_end (& peer -> outgoingCommands), outgoingCommand);
}

ENetOutgoingCommand *
enet_peer_queue_outgoing_command (ENetPeer * peer, const ENetProtocol * command, ENetPacket * packet, enet_uint32 offset, enet_uint16 length)
{
    ENetOutgoingCommand * outgoingCommand = (ENetOutgoingCommand *) enet_malloc (sizeof (ENetOutgoingCommand));
    if (outgoingCommand == NULL)
      return NULL;

    outgoingCommand -> command = * command;
    outgoingCommand -> fragmentOffset = offset;
    outgoingCommand -> fragmentLength = length;
    outgoingCommand -> packet = packet;
    if (packet != NULL)
      ++ packet -> referenceCount;

    enet_peer_setup_outgoing_command (peer, outgoingCommand);

    return outgoingCommand;
}

void
enet_peer_dispatch_incoming_unreliable_commands (ENetPeer * peer, ENetChannel * channel, ENetIncomingCommand * queuedCommand)
{
    ENetListIterator droppedCommand, startCommand, currentCommand;

    for (droppedCommand = startCommand = currentCommand = enet_list_begin (& channel -> incomingUnreliableCommands);
         currentCommand != enet_list_end (& channel -> incomingUnreliableCommands);
         currentCommand = enet_list_next (currentCommand))
    {
       ENetIncomingCommand * incomingCommand = (ENetIncomingCommand *) currentCommand;

       if ((incomingCommand -> command.header.command & ENET_PROTOCOL_COMMAND_MASK) == ENET_PROTOCOL_COMMAND_SEND_UNSEQUENCED)
         continue;

       if (incomingCommand -> reliableSequenceNumber == channel -> incomingReliableSequenceNumber)
       {
          if (incomingCommand -> fragmentsRemaining <= 0)
          {
             channel -> incomingUnreliableSequenceNumber = incomingCommand -> unreliableSequenceNumber;
             continue;
          }

          if (startCommand != currentCommand)
          {
             enet_list_move (enet_list_end (& peer -> dispatchedCommands), startCommand, enet_list_previous (currentCommand));

             if (! (peer -> flags & ENET_PEER_FLAG_NEEDS_DISPATCH))
             {
                enet_list_insert (enet_list_end (& peer -> host -> dispatchQueue), & peer -> dispatchList);

                peer -> flags |= ENET_PEER_FLAG_NEEDS_DISPATCH;
             }

             droppedCommand = currentCommand;
          }
          else
          if (droppedCommand != currentCommand)
            droppedCommand = enet_list_previous (currentCommand);
       }
       else 
       {
          enet_uint16 reliableWindow = incomingCommand -> reliableSequenceNumber / ENET_PEER_RELIABLE_WINDOW_SIZE,
                      currentWindow = channel -> incomingReliableSequenceNumber / ENET_PEER_RELIABLE_WINDOW_SIZE;
          if (incomingCommand -> reliableSequenceNumber < channel -> incomingReliableSequenceNumber)
            reliableWindow += ENET_PEER_RELIABLE_WINDOWS;
          if (reliableWindow >= currentWindow && reliableWindow < currentWindow + ENET_PEER_FREE_RELIABLE_WINDOWS - 1)
            break;

          droppedCommand = enet_list_next (currentCommand);

          if (startCommand != currentCommand)
          {
             enet_list_move (enet_list_end (& peer -> dispatchedCommands), startCommand, enet_list_previous (currentCommand));

             if (! (peer -> flags & ENET_PEER_FLAG_NEEDS_DISPATCH))
             {
                enet_list_insert (enet_list_end (& peer -> host -> dispatchQueue), & peer -> dispatchList);

                peer -> flags |= ENET_PEER_FLAG_NEEDS_DISPATCH;
             }
          }
       }
          
       startCommand = enet_list_next (currentCommand);
    }

    if (startCommand != currentCommand)
    {
       enet_list_move (enet_list_end (& peer -> dispatchedCommands), startCommand, enet_list_previous (currentCommand));

       if (! (peer -> flags & ENET_PEER_FLAG_NEEDS_DISPATCH))
       {
           enet_list_insert (enet_list_end (& peer -> host -> dispatchQueue), & peer -> dispatchList);

           peer -> flags |= ENET_PEER_FLAG_NEEDS_DISPATCH;
       }

       droppedCommand = currentCommand;
    }

    enet_peer_remove_incoming_commands (& channel -> incomingUnreliableCommands, enet_list_begin (& channel -> incomingUnreliableCommands), droppedCommand, queuedCommand);
}

void
enet_peer_dispatch_incoming_reliable_commands (ENetPeer * peer, ENetChannel * channel, ENetIncomingCommand * queuedCommand)
{
    ENetListIterator currentCommand;

    for (currentCommand = enet_list_begin (& channel -> incomingReliableCommands);
         currentCommand != enet_list_end (& channel -> incomingReliableCommands);
         currentCommand = enet_list_next (currentCommand))
    {
       ENetIncomingCommand * incomingCommand = (ENetIncomingCommand *) currentCommand;
         
       if (incomingCommand -> fragmentsRemaining > 0 ||
           incomingCommand -> reliableSequenceNumber != (enet_uint16) (channel -> incomingReliableSequenceNumber + 1))
         break;

       channel -> incomingReliableSequenceNumber = incomingCommand -> reliableSequenceNumber;

       if (incomingCommand -> fragmentCount > 0)
         channel -> incomingReliableSequenceNumber += incomingCommand -> fragmentCount - 1;
    } 

    if (currentCommand == enet_list_begin (& channel -> incomingReliableCommands))
      return;

    channel -> incomingUnreliableSequenceNumber = 0;

    enet_list_move (enet_list_end (& peer -> dispatchedCommands), enet_list_begin (& channel -> incomingReliableCommands), enet_list_previous (currentCommand));

    if (! (peer -> flags & ENET_PEER_FLAG_NEEDS_DISPATCH))
    {
       enet_list_insert (enet_list_end (& peer -> host -> dispatchQueue), & peer -> dispatchList);

       peer -> flags |= ENET_PEER_FLAG_NEEDS_DISPATCH;
    }

    if (! enet_list_empty (& channel -> incomingUnreliableCommands))
       enet_peer_dispatch_incoming_unreliable_commands (peer, channel, queuedCommand);
}

ENetIncomingCommand *
enet_peer_queue_incoming_command (ENetPeer * peer, const ENetProtocol * command, const void * data, size_t dataLength, enet_uint32 flags, enet_uint32 fragmentCount)
{
    static ENetIncomingCommand dummyCommand;

    ENetChannel * channel = & peer -> channels [command -> header.channelID];
    enet_uint32 unreliableSequenceNumber = 0, reliableSequenceNumber = 0;
    enet_uint16 reliableWindow, currentWindow;
    ENetIncomingCommand * incomingCommand;
    ENetListIterator currentCommand;
    ENetPacket * packet = NULL;

    if (peer -> state == ENET_PEER_STATE_DISCONNECT_LATER)
      goto discardCommand;

    if ((command -> header.command & ENET_PROTOCOL_COMMAND_MASK) != ENET_PROTOCOL_COMMAND_SEND_UNSEQUENCED)
    {
        reliableSequenceNumber = command -> header.reliableSequenceNumber;
        reliableWindow = reliableSequenceNumber / ENET_PEER_RELIABLE_WINDOW_SIZE;
        currentWindow = channel -> incomingReliableSequenceNumber / ENET_PEER_RELIABLE_WINDOW_SIZE;

        if (reliableSequenceNumber < channel -> incomingReliableSequenceNumber)
           reliableWindow += ENET_PEER_RELIABLE_WINDOWS;

        if (reliableWindow < currentWindow || reliableWindow >= currentWindow + ENET_PEER_FREE_RELIABLE_WINDOWS - 1)
          goto discardCommand;
    }
                    
    switch (command -> header.command & ENET_PROTOCOL_COMMAND_MASK)
    {
    case ENET_PROTOCOL_COMMAND_SEND_FRAGMENT:
    case ENET_PROTOCOL_COMMAND_SEND_RELIABLE:
       if (reliableSequenceNumber == channel -> incomingReliableSequenceNumber)
         goto discardCommand;
       
       for (currentCommand = enet_list_previous (enet_list_end (& channel -> incomingReliableCommands));
            currentCommand != enet_list_end (& channel -> incomingReliableCommands);
            currentCommand = enet_list_previous (currentCommand))
       {
          incomingCommand = (ENetIncomingCommand *) currentCommand;

          if (reliableSequenceNumber >= channel -> incomingReliableSequenceNumber)
          {
             if (incomingCommand -> reliableSequenceNumber < channel -> incomingReliableSequenceNumber)
               continue;
          }
          else
          if (incomingCommand -> reliableSequenceNumber >= channel -> incomingReliableSequenceNumber)
            break;

          if (incomingCommand -> reliableSequenceNumber <= reliableSequenceNumber)
          {
             if (incomingCommand -> reliableSequenceNumber < reliableSequenceNumber)
               break;

             goto discardCommand;
          }
       }
       break;

    case ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE:
    case ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT:
       unreliableSequenceNumber = ENET_NET_TO_HOST_16 (command -> sendUnreliable.unreliableSequenceNumber);

       if (reliableSequenceNumber == channel -> incomingReliableSequenceNumber && 
           unreliableSequenceNumber <= channel -> incomingUnreliableSequenceNumber)
         goto discardCommand;

       for (currentCommand = enet_list_previous (enet_list_end (& channel -> incomingUnreliableCommands));
            currentCommand != enet_list_end (& channel -> incomingUnreliableCommands);
            currentCommand = enet_list_previous (currentCommand))
       {
          incomingCommand = (ENetIncomingCommand *) currentCommand;

          if ((command -> header.command & ENET_PROTOCOL_COMMAND_MASK) == ENET_PROTOCOL_COMMAND_SEND_UNSEQUENCED)
            continue;

          if (reliableSequenceNumber >= channel -> incomingReliableSequenceNumber)
          {
             if (incomingCommand -> reliableSequenceNumber < channel -> incomingReliableSequenceNumber)
               continue;
          }
          else
          if (incomingCommand -> reliableSequenceNumber >= channel -> incomingReliableSequenceNumber)
            break;

          if (incomingCommand -> reliableSequenceNumber < reliableSequenceNumber)
            break;

          if (incomingCommand -> reliableSequenceNumber > reliableSequenceNumber)
            continue;

          if (incomingCommand -> unreliableSequenceNumber <= unreliableSequenceNumber)
          {
             if (incomingCommand -> unreliableSequenceNumber < unreliableSequenceNumber)
               break;

             goto discardCommand;
          }
       }
       break;

    case ENET_PROTOCOL_COMMAND_SEND_UNSEQUENCED:
       currentCommand = enet_list_end (& channel -> incomingUnreliableCommands);
       break;

    default:
       goto discardCommand;
    }

    if (peer -> totalWaitingData >= peer -> host -> maximumWaitingData)
      goto notifyError;

    packet = enet_packet_create (data, dataLength, flags);
    if (packet == NULL)
      goto notifyError;

    incomingCommand = (ENetIncomingCommand *) enet_malloc (sizeof (ENetIncomingCommand));
    if (incomingCommand == NULL)
      goto notifyError;

    incomingCommand -> reliableSequenceNumber = command -> header.reliableSequenceNumber;
    incomingCommand -> unreliableSequenceNumber = unreliableSequenceNumber & 0xFFFF;
    incomingCommand -> command = * command;
    incomingCommand -> fragmentCount = fragmentCount;
    incomingCommand -> fragmentsRemaining = fragmentCount;
    incomingCommand -> packet = packet;
    incomingCommand -> fragments = NULL;
    
    if (fragmentCount > 0)
    { 
       if (fragmentCount <= ENET_PROTOCOL_MAXIMUM_FRAGMENT_COUNT)
         incomingCommand -> fragments = (enet_uint32 *) enet_malloc ((fragmentCount + 31) / 32 * sizeof (enet_uint32));
       if (incomingCommand -> fragments == NULL)
       {
          enet_free (incomingCommand);

          goto notifyError;
       }
       memset (incomingCommand -> fragments, 0, (fragmentCount + 31) / 32 * sizeof (enet_uint32));
    }

    if (packet != NULL)
    {
       ++ packet -> referenceCount;
      
       peer -> totalWaitingData += packet -> dataLength;
    }

    enet_list_insert (enet_list_next (currentCommand), incomingCommand);

    switch (command -> header.command & ENET_PROTOCOL_COMMAND_MASK)
    {
    case ENET_PROTOCOL_COMMAND_SEND_FRAGMENT:
    case ENET_PROTOCOL_COMMAND_SEND_RELIABLE:
       enet_peer_dispatch_incoming_reliable_commands (peer, channel, incomingCommand);
       break;

    default:
       enet_peer_dispatch_incoming_unreliable_commands (peer, channel, incomingCommand);
       break;
    }

    return incomingCommand;

discardCommand:
    if (fragmentCount > 0)
      goto notifyError;

    if (packet != NULL && packet -> referenceCount == 0)
      enet_packet_destroy (packet);

    return & dummyCommand;

notifyError:
    if (packet != NULL && packet -> referenceCount == 0)
      enet_packet_destroy (packet);

    return NULL;
}

// ----- protocol.c -----

static const size_t commandSizes [ENET_PROTOCOL_COMMAND_COUNT] =
{
    0,
    sizeof (ENetProtocolAcknowledge),
    sizeof (ENetProtocolConnect),
    sizeof (ENetProtocolVerifyConnect),
    sizeof (ENetProtocolDisconnect),
    sizeof (ENetProtocolPing),
    sizeof (ENetProtocolSendReliable),
    sizeof (ENetProtocolSendUnreliable),
    sizeof (ENetProtocolSendFragment),
    sizeof (ENetProtocolSendUnsequenced),
    sizeof (ENetProtocolBandwidthLimit),
    sizeof (ENetProtocolThrottleConfigure),
    sizeof (ENetProtocolSendFragment)
};

size_t
enet_protocol_command_size (enet_uint8 commandNumber)
{
    return commandSizes [commandNumber & ENET_PROTOCOL_COMMAND_MASK];
}

static void
enet_protocol_change_state (ENetHost * host, ENetPeer * peer, ENetPeerState state)
{
    if (state == ENET_PEER_STATE_CONNECTED || state == ENET_PEER_STATE_DISCONNECT_LATER)
      enet_peer_on_connect (peer);
    else
      enet_peer_on_disconnect (peer);

    peer -> state = state;
}

static void
enet_protocol_dispatch_state (ENetHost * host, ENetPeer * peer, ENetPeerState state)
{
    enet_protocol_change_state (host, peer, state);

    if (! (peer -> flags & ENET_PEER_FLAG_NEEDS_DISPATCH))
    {
       enet_list_insert (enet_list_end (& host -> dispatchQueue), & peer -> dispatchList);

       peer -> flags |= ENET_PEER_FLAG_NEEDS_DISPATCH;
    }
}

static int
enet_protocol_dispatch_incoming_commands (ENetHost * host, ENetEvent * event)
{
    while (! enet_list_empty (& host -> dispatchQueue))
    {
       ENetPeer * peer = (ENetPeer *) enet_list_remove (enet_list_begin (& host -> dispatchQueue));

       peer -> flags &= ~ ENET_PEER_FLAG_NEEDS_DISPATCH;

       switch (peer -> state)
       {
       case ENET_PEER_STATE_CONNECTION_PENDING:
       case ENET_PEER_STATE_CONNECTION_SUCCEEDED:
           enet_protocol_change_state (host, peer, ENET_PEER_STATE_CONNECTED);

           event -> type = ENET_EVENT_TYPE_CONNECT;
           event -> peer = peer;
           event -> data = peer -> eventData;

           return 1;
           
       case ENET_PEER_STATE_ZOMBIE:
           host -> recalculateBandwidthLimits = 1;

           event -> type = ENET_EVENT_TYPE_DISCONNECT;
           event -> peer = peer;
           event -> data = peer -> eventData;

           enet_peer_reset (peer);

           return 1;

       case ENET_PEER_STATE_CONNECTED:
           if (enet_list_empty (& peer -> dispatchedCommands))
             continue;

           event -> packet = enet_peer_receive (peer, & event -> channelID);
           if (event -> packet == NULL)
             continue;
             
           event -> type = ENET_EVENT_TYPE_RECEIVE;
           event -> peer = peer;

           if (! enet_list_empty (& peer -> dispatchedCommands))
           {
              peer -> flags |= ENET_PEER_FLAG_NEEDS_DISPATCH;
         
              enet_list_insert (enet_list_end (& host -> dispatchQueue), & peer -> dispatchList);
           }

           return 1;

       default:
           break;
       }
    }

    return 0;
}

static void
enet_protocol_notify_connect (ENetHost * host, ENetPeer * peer, ENetEvent * event)
{
    host -> recalculateBandwidthLimits = 1;

    if (event != NULL)
    {
        enet_protocol_change_state (host, peer, ENET_PEER_STATE_CONNECTED);

        event -> type = ENET_EVENT_TYPE_CONNECT;
        event -> peer = peer;
        event -> data = peer -> eventData;
    }
    else 
        enet_protocol_dispatch_state (host, peer, peer -> state == ENET_PEER_STATE_CONNECTING ? ENET_PEER_STATE_CONNECTION_SUCCEEDED : ENET_PEER_STATE_CONNECTION_PENDING);
}

static void
enet_protocol_notify_disconnect (ENetHost * host, ENetPeer * peer, ENetEvent * event)
{
    if (peer -> state >= ENET_PEER_STATE_CONNECTION_PENDING)
       host -> recalculateBandwidthLimits = 1;

    if (peer -> state != ENET_PEER_STATE_CONNECTING && peer -> state < ENET_PEER_STATE_CONNECTION_SUCCEEDED)
        enet_peer_reset (peer);
    else
    if (event != NULL)
    {
        event -> type = ENET_EVENT_TYPE_DISCONNECT;
        event -> peer = peer;
        event -> data = 0;

        enet_peer_reset (peer);
    }
    else 
    {
        peer -> eventData = 0;

        enet_protocol_dispatch_state (host, peer, ENET_PEER_STATE_ZOMBIE);
    }
}

static void
enet_protocol_remove_sent_unreliable_commands (ENetPeer * peer)
{
    ENetOutgoingCommand * outgoingCommand;

    if (enet_list_empty (& peer -> sentUnreliableCommands))
      return;

    do
    {
        outgoingCommand = (ENetOutgoingCommand *) enet_list_front (& peer -> sentUnreliableCommands);
        
        enet_list_remove (& outgoingCommand -> outgoingCommandList);

        if (outgoingCommand -> packet != NULL)
        {
           -- outgoingCommand -> packet -> referenceCount;

           if (outgoingCommand -> packet -> referenceCount == 0)
           {
              outgoingCommand -> packet -> flags |= ENET_PACKET_FLAG_SENT;
 
              enet_packet_destroy (outgoingCommand -> packet);
           }
        }

        enet_free (outgoingCommand);
    } while (! enet_list_empty (& peer -> sentUnreliableCommands));

    if (peer -> state == ENET_PEER_STATE_DISCONNECT_LATER &&
        enet_list_empty (& peer -> outgoingCommands) &&
        enet_list_empty (& peer -> sentReliableCommands))
      enet_peer_disconnect (peer, peer -> eventData);
}

static ENetProtocolCommand
enet_protocol_remove_sent_reliable_command (ENetPeer * peer, enet_uint16 reliableSequenceNumber, enet_uint8 channelID)
{
    ENetOutgoingCommand * outgoingCommand = NULL;
    ENetListIterator currentCommand;
    ENetProtocolCommand commandNumber;
    int wasSent = 1;

    for (currentCommand = enet_list_begin (& peer -> sentReliableCommands);
         currentCommand != enet_list_end (& peer -> sentReliableCommands);
         currentCommand = enet_list_next (currentCommand))
    {
       outgoingCommand = (ENetOutgoingCommand *) currentCommand;
        
       if (outgoingCommand -> reliableSequenceNumber == reliableSequenceNumber &&
           outgoingCommand -> command.header.channelID == channelID)
         break;
    }

    if (currentCommand == enet_list_end (& peer -> sentReliableCommands))
    {
       for (currentCommand = enet_list_begin (& peer -> outgoingCommands);
            currentCommand != enet_list_end (& peer -> outgoingCommands);
            currentCommand = enet_list_next (currentCommand))
       {
          outgoingCommand = (ENetOutgoingCommand *) currentCommand;

          if (! (outgoingCommand -> command.header.command & ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE))
            continue;

          if (outgoingCommand -> sendAttempts < 1) return ENET_PROTOCOL_COMMAND_NONE;

          if (outgoingCommand -> reliableSequenceNumber == reliableSequenceNumber &&
              outgoingCommand -> command.header.channelID == channelID)
            break;
       }

       if (currentCommand == enet_list_end (& peer -> outgoingCommands))
         return ENET_PROTOCOL_COMMAND_NONE;

       wasSent = 0;
    }

    if (outgoingCommand == NULL)
      return ENET_PROTOCOL_COMMAND_NONE;

    if (channelID < peer -> channelCount)
    {
       ENetChannel * channel = & peer -> channels [channelID];
       enet_uint16 reliableWindow = reliableSequenceNumber / ENET_PEER_RELIABLE_WINDOW_SIZE;
       if (channel -> reliableWindows [reliableWindow] > 0)
       {
          -- channel -> reliableWindows [reliableWindow];
          if (! channel -> reliableWindows [reliableWindow])
            channel -> usedReliableWindows &= ~ (1 << reliableWindow);
       }
    }

    commandNumber = (ENetProtocolCommand) (outgoingCommand -> command.header.command & ENET_PROTOCOL_COMMAND_MASK);
    
    enet_list_remove (& outgoingCommand -> outgoingCommandList);

    if (outgoingCommand -> packet != NULL)
    {
       if (wasSent)
         peer -> reliableDataInTransit -= outgoingCommand -> fragmentLength;

       -- outgoingCommand -> packet -> referenceCount;

       if (outgoingCommand -> packet -> referenceCount == 0)
       {
          outgoingCommand -> packet -> flags |= ENET_PACKET_FLAG_SENT;

          enet_packet_destroy (outgoingCommand -> packet);
       }
    }

    enet_free (outgoingCommand);

    if (enet_list_empty (& peer -> sentReliableCommands))
      return commandNumber;
    
    outgoingCommand = (ENetOutgoingCommand *) enet_list_front (& peer -> sentReliableCommands);
    
    peer -> nextTimeout = outgoingCommand -> sentTime + outgoingCommand -> roundTripTimeout;

    return commandNumber;
} 

static ENetPeer *
enet_protocol_handle_connect (ENetHost * host, ENetProtocolHeader * header, ENetProtocol * command)
{
    enet_uint8 incomingSessionID, outgoingSessionID;
    enet_uint32 mtu, windowSize;
    ENetChannel * channel;
    size_t channelCount, duplicatePeers = 0;
    ENetPeer * currentPeer, * peer = NULL;
    ENetProtocol verifyCommand;

    channelCount = ENET_NET_TO_HOST_32 (command -> connect.channelCount);

    if (channelCount < ENET_PROTOCOL_MINIMUM_CHANNEL_COUNT ||
        channelCount > ENET_PROTOCOL_MAXIMUM_CHANNEL_COUNT)
      return NULL;

    for (currentPeer = host -> peers;
         currentPeer < & host -> peers [host -> peerCount];
         ++ currentPeer)
    {
        if (currentPeer -> state == ENET_PEER_STATE_DISCONNECTED)
        {
            if (peer == NULL)
              peer = currentPeer;
        }
        else 
        if (currentPeer -> state != ENET_PEER_STATE_CONNECTING &&
            currentPeer -> address.host == host -> receivedAddress.host)
        {
            if (currentPeer -> address.port == host -> receivedAddress.port &&
                currentPeer -> connectID == command -> connect.connectID)
              return NULL;

            ++ duplicatePeers;
        }
    }

    if (peer == NULL || duplicatePeers >= host -> duplicatePeers)
      return NULL;

    if (channelCount > host -> channelLimit)
      channelCount = host -> channelLimit;
    peer -> channels = (ENetChannel *) enet_malloc (channelCount * sizeof (ENetChannel));
    if (peer -> channels == NULL)
      return NULL;
    peer -> channelCount = channelCount;
    peer -> state = ENET_PEER_STATE_ACKNOWLEDGING_CONNECT;
    peer -> connectID = command -> connect.connectID;
    peer -> address = host -> receivedAddress;
    peer -> outgoingPeerID = ENET_NET_TO_HOST_16 (command -> connect.outgoingPeerID);
    peer -> incomingBandwidth = ENET_NET_TO_HOST_32 (command -> connect.incomingBandwidth);
    peer -> outgoingBandwidth = ENET_NET_TO_HOST_32 (command -> connect.outgoingBandwidth);
    peer -> packetThrottleInterval = ENET_NET_TO_HOST_32 (command -> connect.packetThrottleInterval);
    peer -> packetThrottleAcceleration = ENET_NET_TO_HOST_32 (command -> connect.packetThrottleAcceleration);
    peer -> packetThrottleDeceleration = ENET_NET_TO_HOST_32 (command -> connect.packetThrottleDeceleration);
    peer -> eventData = ENET_NET_TO_HOST_32 (command -> connect.data);

    incomingSessionID = command -> connect.incomingSessionID == 0xFF ? peer -> outgoingSessionID : command -> connect.incomingSessionID;
    incomingSessionID = (incomingSessionID + 1) & (ENET_PROTOCOL_HEADER_SESSION_MASK >> ENET_PROTOCOL_HEADER_SESSION_SHIFT);
    if (incomingSessionID == peer -> outgoingSessionID)
      incomingSessionID = (incomingSessionID + 1) & (ENET_PROTOCOL_HEADER_SESSION_MASK >> ENET_PROTOCOL_HEADER_SESSION_SHIFT);
    peer -> outgoingSessionID = incomingSessionID;

    outgoingSessionID = command -> connect.outgoingSessionID == 0xFF ? peer -> incomingSessionID : command -> connect.outgoingSessionID;
    outgoingSessionID = (outgoingSessionID + 1) & (ENET_PROTOCOL_HEADER_SESSION_MASK >> ENET_PROTOCOL_HEADER_SESSION_SHIFT);
    if (outgoingSessionID == peer -> incomingSessionID)
      outgoingSessionID = (outgoingSessionID + 1) & (ENET_PROTOCOL_HEADER_SESSION_MASK >> ENET_PROTOCOL_HEADER_SESSION_SHIFT);
    peer -> incomingSessionID = outgoingSessionID;

    for (channel = peer -> channels;
         channel < & peer -> channels [channelCount];
         ++ channel)
    {
        channel -> outgoingReliableSequenceNumber = 0;
        channel -> outgoingUnreliableSequenceNumber = 0;
        channel -> incomingReliableSequenceNumber = 0;
        channel -> incomingUnreliableSequenceNumber = 0;

        enet_list_clear (& channel -> incomingReliableCommands);
        enet_list_clear (& channel -> incomingUnreliableCommands);

        channel -> usedReliableWindows = 0;
        memset (channel -> reliableWindows, 0, sizeof (channel -> reliableWindows));
    }

    mtu = ENET_NET_TO_HOST_32 (command -> connect.mtu);

    if (mtu < ENET_PROTOCOL_MINIMUM_MTU)
      mtu = ENET_PROTOCOL_MINIMUM_MTU;
    else
    if (mtu > ENET_PROTOCOL_MAXIMUM_MTU)
      mtu = ENET_PROTOCOL_MAXIMUM_MTU;

    peer -> mtu = mtu;

    if (host -> outgoingBandwidth == 0 &&
        peer -> incomingBandwidth == 0)
      peer -> windowSize = ENET_PROTOCOL_MAXIMUM_WINDOW_SIZE;
    else
    if (host -> outgoingBandwidth == 0 ||
        peer -> incomingBandwidth == 0)
      peer -> windowSize = (ENET_MAX (host -> outgoingBandwidth, peer -> incomingBandwidth) /
                                    ENET_PEER_WINDOW_SIZE_SCALE) *
                                      ENET_PROTOCOL_MINIMUM_WINDOW_SIZE;
    else
      peer -> windowSize = (ENET_MIN (host -> outgoingBandwidth, peer -> incomingBandwidth) /
                                    ENET_PEER_WINDOW_SIZE_SCALE) * 
                                      ENET_PROTOCOL_MINIMUM_WINDOW_SIZE;

    if (peer -> windowSize < ENET_PROTOCOL_MINIMUM_WINDOW_SIZE)
      peer -> windowSize = ENET_PROTOCOL_MINIMUM_WINDOW_SIZE;
    else
    if (peer -> windowSize > ENET_PROTOCOL_MAXIMUM_WINDOW_SIZE)
      peer -> windowSize = ENET_PROTOCOL_MAXIMUM_WINDOW_SIZE;

    if (host -> incomingBandwidth == 0)
      windowSize = ENET_PROTOCOL_MAXIMUM_WINDOW_SIZE;
    else
      windowSize = (host -> incomingBandwidth / ENET_PEER_WINDOW_SIZE_SCALE) *
                     ENET_PROTOCOL_MINIMUM_WINDOW_SIZE;

    if (windowSize > ENET_NET_TO_HOST_32 (command -> connect.windowSize))
      windowSize = ENET_NET_TO_HOST_32 (command -> connect.windowSize);

    if (windowSize < ENET_PROTOCOL_MINIMUM_WINDOW_SIZE)
      windowSize = ENET_PROTOCOL_MINIMUM_WINDOW_SIZE;
    else
    if (windowSize > ENET_PROTOCOL_MAXIMUM_WINDOW_SIZE)
      windowSize = ENET_PROTOCOL_MAXIMUM_WINDOW_SIZE;

    verifyCommand.header.command = ENET_PROTOCOL_COMMAND_VERIFY_CONNECT | ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE;
    verifyCommand.header.channelID = 0xFF;
    verifyCommand.verifyConnect.outgoingPeerID = ENET_HOST_TO_NET_16 (peer -> incomingPeerID);
    verifyCommand.verifyConnect.incomingSessionID = incomingSessionID;
    verifyCommand.verifyConnect.outgoingSessionID = outgoingSessionID;
    verifyCommand.verifyConnect.mtu = ENET_HOST_TO_NET_32 (peer -> mtu);
    verifyCommand.verifyConnect.windowSize = ENET_HOST_TO_NET_32 (windowSize);
    verifyCommand.verifyConnect.channelCount = ENET_HOST_TO_NET_32 (channelCount);
    verifyCommand.verifyConnect.incomingBandwidth = ENET_HOST_TO_NET_32 (host -> incomingBandwidth);
    verifyCommand.verifyConnect.outgoingBandwidth = ENET_HOST_TO_NET_32 (host -> outgoingBandwidth);
    verifyCommand.verifyConnect.packetThrottleInterval = ENET_HOST_TO_NET_32 (peer -> packetThrottleInterval);
    verifyCommand.verifyConnect.packetThrottleAcceleration = ENET_HOST_TO_NET_32 (peer -> packetThrottleAcceleration);
    verifyCommand.verifyConnect.packetThrottleDeceleration = ENET_HOST_TO_NET_32 (peer -> packetThrottleDeceleration);
    verifyCommand.verifyConnect.connectID = peer -> connectID;

    enet_peer_queue_outgoing_command (peer, & verifyCommand, NULL, 0, 0);

    return peer;
}

static int
enet_protocol_handle_send_reliable (ENetHost * host, ENetPeer * peer, const ENetProtocol * command, enet_uint8 ** currentData)
{
    size_t dataLength;

    if (command -> header.channelID >= peer -> channelCount ||
        (peer -> state != ENET_PEER_STATE_CONNECTED && peer -> state != ENET_PEER_STATE_DISCONNECT_LATER))
      return -1;

    dataLength = ENET_NET_TO_HOST_16 (command -> sendReliable.dataLength);
    * currentData += dataLength;
    if (dataLength > host -> maximumPacketSize ||
        * currentData < host -> receivedData ||
        * currentData > & host -> receivedData [host -> receivedDataLength])
      return -1;

    if (enet_peer_queue_incoming_command (peer, command, (const enet_uint8 *) command + sizeof (ENetProtocolSendReliable), dataLength, ENET_PACKET_FLAG_RELIABLE, 0) == NULL)
      return -1;

    return 0;
}

static int
enet_protocol_handle_send_unsequenced (ENetHost * host, ENetPeer * peer, const ENetProtocol * command, enet_uint8 ** currentData)
{
    enet_uint32 unsequencedGroup, index;
    size_t dataLength;

    if (command -> header.channelID >= peer -> channelCount ||
        (peer -> state != ENET_PEER_STATE_CONNECTED && peer -> state != ENET_PEER_STATE_DISCONNECT_LATER))
      return -1;

    dataLength = ENET_NET_TO_HOST_16 (command -> sendUnsequenced.dataLength);
    * currentData += dataLength;
    if (dataLength > host -> maximumPacketSize ||
        * currentData < host -> receivedData ||
        * currentData > & host -> receivedData [host -> receivedDataLength])
      return -1; 

    unsequencedGroup = ENET_NET_TO_HOST_16 (command -> sendUnsequenced.unsequencedGroup);
    index = unsequencedGroup % ENET_PEER_UNSEQUENCED_WINDOW_SIZE;
   
    if (unsequencedGroup < peer -> incomingUnsequencedGroup)
      unsequencedGroup += 0x10000;

    if (unsequencedGroup >= (enet_uint32) peer -> incomingUnsequencedGroup + ENET_PEER_FREE_UNSEQUENCED_WINDOWS * ENET_PEER_UNSEQUENCED_WINDOW_SIZE)
      return 0;

    unsequencedGroup &= 0xFFFF;

    if (unsequencedGroup - index != peer -> incomingUnsequencedGroup)
    {
        peer -> incomingUnsequencedGroup = unsequencedGroup - index;

        memset (peer -> unsequencedWindow, 0, sizeof (peer -> unsequencedWindow));
    }
    else
    if (peer -> unsequencedWindow [index / 32] & (1 << (index % 32)))
      return 0;
      
    if (enet_peer_queue_incoming_command (peer, command, (const enet_uint8 *) command + sizeof (ENetProtocolSendUnsequenced), dataLength, ENET_PACKET_FLAG_UNSEQUENCED, 0) == NULL)
      return -1;
   
    peer -> unsequencedWindow [index / 32] |= 1 << (index % 32);
 
    return 0;
}

static int
enet_protocol_handle_send_unreliable (ENetHost * host, ENetPeer * peer, const ENetProtocol * command, enet_uint8 ** currentData)
{
    size_t dataLength;

    if (command -> header.channelID >= peer -> channelCount ||
        (peer -> state != ENET_PEER_STATE_CONNECTED && peer -> state != ENET_PEER_STATE_DISCONNECT_LATER))
      return -1;

    dataLength = ENET_NET_TO_HOST_16 (command -> sendUnreliable.dataLength);
    * currentData += dataLength;
    if (dataLength > host -> maximumPacketSize ||
        * currentData < host -> receivedData ||
        * currentData > & host -> receivedData [host -> receivedDataLength])
      return -1;

    if (enet_peer_queue_incoming_command (peer, command, (const enet_uint8 *) command + sizeof (ENetProtocolSendUnreliable), dataLength, 0, 0) == NULL)
      return -1;

    return 0;
}

static int
enet_protocol_handle_send_fragment (ENetHost * host, ENetPeer * peer, const ENetProtocol * command, enet_uint8 ** currentData)
{
    enet_uint32 fragmentNumber,
           fragmentCount,
           fragmentOffset,
           fragmentLength,
           startSequenceNumber,
           totalLength;
    ENetChannel * channel;
    enet_uint16 startWindow, currentWindow;
    ENetListIterator currentCommand;
    ENetIncomingCommand * startCommand = NULL;

    if (command -> header.channelID >= peer -> channelCount ||
        (peer -> state != ENET_PEER_STATE_CONNECTED && peer -> state != ENET_PEER_STATE_DISCONNECT_LATER))
      return -1;

    fragmentLength = ENET_NET_TO_HOST_16 (command -> sendFragment.dataLength);
    * currentData += fragmentLength;
    if (fragmentLength > host -> maximumPacketSize ||
        * currentData < host -> receivedData ||
        * currentData > & host -> receivedData [host -> receivedDataLength])
      return -1;

    channel = & peer -> channels [command -> header.channelID];
    startSequenceNumber = ENET_NET_TO_HOST_16 (command -> sendFragment.startSequenceNumber);
    startWindow = startSequenceNumber / ENET_PEER_RELIABLE_WINDOW_SIZE;
    currentWindow = channel -> incomingReliableSequenceNumber / ENET_PEER_RELIABLE_WINDOW_SIZE;

    if (startSequenceNumber < channel -> incomingReliableSequenceNumber)
      startWindow += ENET_PEER_RELIABLE_WINDOWS;

    if (startWindow < currentWindow || startWindow >= currentWindow + ENET_PEER_FREE_RELIABLE_WINDOWS - 1)
      return 0;

    fragmentNumber = ENET_NET_TO_HOST_32 (command -> sendFragment.fragmentNumber);
    fragmentCount = ENET_NET_TO_HOST_32 (command -> sendFragment.fragmentCount);
    fragmentOffset = ENET_NET_TO_HOST_32 (command -> sendFragment.fragmentOffset);
    totalLength = ENET_NET_TO_HOST_32 (command -> sendFragment.totalLength);
    
    if (fragmentCount > ENET_PROTOCOL_MAXIMUM_FRAGMENT_COUNT ||
        fragmentNumber >= fragmentCount ||
        totalLength > host -> maximumPacketSize ||
        fragmentOffset >= totalLength ||
        fragmentLength > totalLength - fragmentOffset)
      return -1;
 
    for (currentCommand = enet_list_previous (enet_list_end (& channel -> incomingReliableCommands));
         currentCommand != enet_list_end (& channel -> incomingReliableCommands);
         currentCommand = enet_list_previous (currentCommand))
    {
       ENetIncomingCommand * incomingCommand = (ENetIncomingCommand *) currentCommand;

       if (startSequenceNumber >= channel -> incomingReliableSequenceNumber)
       {
          if (incomingCommand -> reliableSequenceNumber < channel -> incomingReliableSequenceNumber)
            continue;
       }
       else
       if (incomingCommand -> reliableSequenceNumber >= channel -> incomingReliableSequenceNumber)
         break;

       if (incomingCommand -> reliableSequenceNumber <= startSequenceNumber)
       {
          if (incomingCommand -> reliableSequenceNumber < startSequenceNumber)
            break;
        
          if ((incomingCommand -> command.header.command & ENET_PROTOCOL_COMMAND_MASK) != ENET_PROTOCOL_COMMAND_SEND_FRAGMENT ||
              totalLength != incomingCommand -> packet -> dataLength ||
              fragmentCount != incomingCommand -> fragmentCount)
            return -1;

          startCommand = incomingCommand;
          break;
       }
    }
 
    if (startCommand == NULL)
    {
       ENetProtocol hostCommand = * command;

       hostCommand.header.reliableSequenceNumber = startSequenceNumber;

       startCommand = enet_peer_queue_incoming_command (peer, & hostCommand, NULL, totalLength, ENET_PACKET_FLAG_RELIABLE, fragmentCount);
       if (startCommand == NULL)
         return -1;
    }
    
    if ((startCommand -> fragments [fragmentNumber / 32] & (1 << (fragmentNumber % 32))) == 0)
    {
       -- startCommand -> fragmentsRemaining;

       startCommand -> fragments [fragmentNumber / 32] |= (1 << (fragmentNumber % 32));

       if (fragmentOffset + fragmentLength > startCommand -> packet -> dataLength)
         fragmentLength = startCommand -> packet -> dataLength - fragmentOffset;

       memcpy (startCommand -> packet -> data + fragmentOffset,
               (enet_uint8 *) command + sizeof (ENetProtocolSendFragment),
               fragmentLength);

        if (startCommand -> fragmentsRemaining <= 0)
          enet_peer_dispatch_incoming_reliable_commands (peer, channel, NULL);
    }

    return 0;
}

static int
enet_protocol_handle_send_unreliable_fragment (ENetHost * host, ENetPeer * peer, const ENetProtocol * command, enet_uint8 ** currentData)
{
    enet_uint32 fragmentNumber,
           fragmentCount,
           fragmentOffset,
           fragmentLength,
           reliableSequenceNumber,
           startSequenceNumber,
           totalLength;
    enet_uint16 reliableWindow, currentWindow;
    ENetChannel * channel;
    ENetListIterator currentCommand;
    ENetIncomingCommand * startCommand = NULL;

    if (command -> header.channelID >= peer -> channelCount ||
        (peer -> state != ENET_PEER_STATE_CONNECTED && peer -> state != ENET_PEER_STATE_DISCONNECT_LATER))
      return -1;

    fragmentLength = ENET_NET_TO_HOST_16 (command -> sendFragment.dataLength);
    * currentData += fragmentLength;
    if (fragmentLength > host -> maximumPacketSize ||
        * currentData < host -> receivedData ||
        * currentData > & host -> receivedData [host -> receivedDataLength])
      return -1;

    channel = & peer -> channels [command -> header.channelID];
    reliableSequenceNumber = command -> header.reliableSequenceNumber;
    startSequenceNumber = ENET_NET_TO_HOST_16 (command -> sendFragment.startSequenceNumber);

    reliableWindow = reliableSequenceNumber / ENET_PEER_RELIABLE_WINDOW_SIZE;
    currentWindow = channel -> incomingReliableSequenceNumber / ENET_PEER_RELIABLE_WINDOW_SIZE;

    if (reliableSequenceNumber < channel -> incomingReliableSequenceNumber)
      reliableWindow += ENET_PEER_RELIABLE_WINDOWS;

    if (reliableWindow < currentWindow || reliableWindow >= currentWindow + ENET_PEER_FREE_RELIABLE_WINDOWS - 1)
      return 0;

    if (reliableSequenceNumber == channel -> incomingReliableSequenceNumber &&
        startSequenceNumber <= channel -> incomingUnreliableSequenceNumber)
      return 0;

    fragmentNumber = ENET_NET_TO_HOST_32 (command -> sendFragment.fragmentNumber);
    fragmentCount = ENET_NET_TO_HOST_32 (command -> sendFragment.fragmentCount);
    fragmentOffset = ENET_NET_TO_HOST_32 (command -> sendFragment.fragmentOffset);
    totalLength = ENET_NET_TO_HOST_32 (command -> sendFragment.totalLength);

    if (fragmentCount > ENET_PROTOCOL_MAXIMUM_FRAGMENT_COUNT ||
        fragmentNumber >= fragmentCount ||
        totalLength > host -> maximumPacketSize ||
        fragmentOffset >= totalLength ||
        fragmentLength > totalLength - fragmentOffset)
      return -1;

    for (currentCommand = enet_list_previous (enet_list_end (& channel -> incomingUnreliableCommands));
         currentCommand != enet_list_end (& channel -> incomingUnreliableCommands);
         currentCommand = enet_list_previous (currentCommand))
    {
       ENetIncomingCommand * incomingCommand = (ENetIncomingCommand *) currentCommand;

       if (reliableSequenceNumber >= channel -> incomingReliableSequenceNumber)
       {
          if (incomingCommand -> reliableSequenceNumber < channel -> incomingReliableSequenceNumber)
            continue;
       }
       else
       if (incomingCommand -> reliableSequenceNumber >= channel -> incomingReliableSequenceNumber)
         break;

       if (incomingCommand -> reliableSequenceNumber < reliableSequenceNumber)
         break;

       if (incomingCommand -> reliableSequenceNumber > reliableSequenceNumber)
         continue;

       if (incomingCommand -> unreliableSequenceNumber <= startSequenceNumber)
       {
          if (incomingCommand -> unreliableSequenceNumber < startSequenceNumber)
            break;

          if ((incomingCommand -> command.header.command & ENET_PROTOCOL_COMMAND_MASK) != ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT ||
              totalLength != incomingCommand -> packet -> dataLength ||
              fragmentCount != incomingCommand -> fragmentCount)
            return -1;

          startCommand = incomingCommand;
          break;
       }
    }

    if (startCommand == NULL)
    {
       startCommand = enet_peer_queue_incoming_command (peer, command, NULL, totalLength, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT, fragmentCount);
       if (startCommand == NULL)
         return -1;
    }

    if ((startCommand -> fragments [fragmentNumber / 32] & (1 << (fragmentNumber % 32))) == 0)
    {
       -- startCommand -> fragmentsRemaining;

       startCommand -> fragments [fragmentNumber / 32] |= (1 << (fragmentNumber % 32));

       if (fragmentOffset + fragmentLength > startCommand -> packet -> dataLength)
         fragmentLength = startCommand -> packet -> dataLength - fragmentOffset;

       memcpy (startCommand -> packet -> data + fragmentOffset,
               (enet_uint8 *) command + sizeof (ENetProtocolSendFragment),
               fragmentLength);

        if (startCommand -> fragmentsRemaining <= 0)
          enet_peer_dispatch_incoming_unreliable_commands (peer, channel, NULL);
    }

    return 0;
}

static int
enet_protocol_handle_ping (ENetHost * host, ENetPeer * peer, const ENetProtocol * command)
{
    if (peer -> state != ENET_PEER_STATE_CONNECTED && peer -> state != ENET_PEER_STATE_DISCONNECT_LATER)
      return -1;

    return 0;
}

static int
enet_protocol_handle_bandwidth_limit (ENetHost * host, ENetPeer * peer, const ENetProtocol * command)
{
    if (peer -> state != ENET_PEER_STATE_CONNECTED && peer -> state != ENET_PEER_STATE_DISCONNECT_LATER)
      return -1;

    if (peer -> incomingBandwidth != 0)
      -- host -> bandwidthLimitedPeers;

    peer -> incomingBandwidth = ENET_NET_TO_HOST_32 (command -> bandwidthLimit.incomingBandwidth);
    peer -> outgoingBandwidth = ENET_NET_TO_HOST_32 (command -> bandwidthLimit.outgoingBandwidth);

    if (peer -> incomingBandwidth != 0)
      ++ host -> bandwidthLimitedPeers;

    if (peer -> incomingBandwidth == 0 && host -> outgoingBandwidth == 0)
      peer -> windowSize = ENET_PROTOCOL_MAXIMUM_WINDOW_SIZE;
    else
    if (peer -> incomingBandwidth == 0 || host -> outgoingBandwidth == 0)
      peer -> windowSize = (ENET_MAX (peer -> incomingBandwidth, host -> outgoingBandwidth) /
                             ENET_PEER_WINDOW_SIZE_SCALE) * ENET_PROTOCOL_MINIMUM_WINDOW_SIZE;
    else
      peer -> windowSize = (ENET_MIN (peer -> incomingBandwidth, host -> outgoingBandwidth) /
                             ENET_PEER_WINDOW_SIZE_SCALE) * ENET_PROTOCOL_MINIMUM_WINDOW_SIZE;

    if (peer -> windowSize < ENET_PROTOCOL_MINIMUM_WINDOW_SIZE)
      peer -> windowSize = ENET_PROTOCOL_MINIMUM_WINDOW_SIZE;
    else
    if (peer -> windowSize > ENET_PROTOCOL_MAXIMUM_WINDOW_SIZE)
      peer -> windowSize = ENET_PROTOCOL_MAXIMUM_WINDOW_SIZE;

    return 0;
}

static int
enet_protocol_handle_throttle_configure (ENetHost * host, ENetPeer * peer, const ENetProtocol * command)
{
    if (peer -> state != ENET_PEER_STATE_CONNECTED && peer -> state != ENET_PEER_STATE_DISCONNECT_LATER)
      return -1;

    peer -> packetThrottleInterval = ENET_NET_TO_HOST_32 (command -> throttleConfigure.packetThrottleInterval);
    peer -> packetThrottleAcceleration = ENET_NET_TO_HOST_32 (command -> throttleConfigure.packetThrottleAcceleration);
    peer -> packetThrottleDeceleration = ENET_NET_TO_HOST_32 (command -> throttleConfigure.packetThrottleDeceleration);

    return 0;
}

static int
enet_protocol_handle_disconnect (ENetHost * host, ENetPeer * peer, const ENetProtocol * command)
{
    if (peer -> state == ENET_PEER_STATE_DISCONNECTED || peer -> state == ENET_PEER_STATE_ZOMBIE || peer -> state == ENET_PEER_STATE_ACKNOWLEDGING_DISCONNECT)
      return 0;

    enet_peer_reset_queues (peer);

    if (peer -> state == ENET_PEER_STATE_CONNECTION_SUCCEEDED || peer -> state == ENET_PEER_STATE_DISCONNECTING || peer -> state == ENET_PEER_STATE_CONNECTING)
        enet_protocol_dispatch_state (host, peer, ENET_PEER_STATE_ZOMBIE);
    else
    if (peer -> state != ENET_PEER_STATE_CONNECTED && peer -> state != ENET_PEER_STATE_DISCONNECT_LATER)
    {
        if (peer -> state == ENET_PEER_STATE_CONNECTION_PENDING) host -> recalculateBandwidthLimits = 1;

        enet_peer_reset (peer);
    }
    else
    if (command -> header.command & ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE)
      enet_protocol_change_state (host, peer, ENET_PEER_STATE_ACKNOWLEDGING_DISCONNECT);
    else
      enet_protocol_dispatch_state (host, peer, ENET_PEER_STATE_ZOMBIE);

    if (peer -> state != ENET_PEER_STATE_DISCONNECTED)
      peer -> eventData = ENET_NET_TO_HOST_32 (command -> disconnect.data);

    return 0;
}

static int
enet_protocol_handle_acknowledge (ENetHost * host, ENetEvent * event, ENetPeer * peer, const ENetProtocol * command)
{
    enet_uint32 roundTripTime,
           receivedSentTime,
           receivedReliableSequenceNumber;
    ENetProtocolCommand commandNumber;

    if (peer -> state == ENET_PEER_STATE_DISCONNECTED || peer -> state == ENET_PEER_STATE_ZOMBIE)
      return 0;

    receivedSentTime = ENET_NET_TO_HOST_16 (command -> acknowledge.receivedSentTime);
    receivedSentTime |= host -> serviceTime & 0xFFFF0000;
    if ((receivedSentTime & 0x8000) > (host -> serviceTime & 0x8000))
        receivedSentTime -= 0x10000;

    if (ENET_TIME_LESS (host -> serviceTime, receivedSentTime))
      return 0;

    roundTripTime = ENET_TIME_DIFFERENCE (host -> serviceTime, receivedSentTime);
    roundTripTime = ENET_MAX (roundTripTime, 1);

    if (peer -> lastReceiveTime > 0)
    {
       enet_peer_throttle (peer, roundTripTime);

       peer -> roundTripTimeVariance -= peer -> roundTripTimeVariance / 4;

       if (roundTripTime >= peer -> roundTripTime)
       {
          enet_uint32 diff = roundTripTime - peer -> roundTripTime;
          peer -> roundTripTimeVariance += diff / 4;
          peer -> roundTripTime += diff / 8;
       }
       else
       {
          enet_uint32 diff = peer -> roundTripTime - roundTripTime;
          peer -> roundTripTimeVariance += diff / 4;
          peer -> roundTripTime -= diff / 8;
       }
    }
    else
    {
       peer -> roundTripTime = roundTripTime;
       peer -> roundTripTimeVariance = (roundTripTime + 1) / 2;
    }

    if (peer -> roundTripTime < peer -> lowestRoundTripTime)
      peer -> lowestRoundTripTime = peer -> roundTripTime;

    if (peer -> roundTripTimeVariance > peer -> highestRoundTripTimeVariance)
      peer -> highestRoundTripTimeVariance = peer -> roundTripTimeVariance;

    if (peer -> packetThrottleEpoch == 0 ||
        ENET_TIME_DIFFERENCE (host -> serviceTime, peer -> packetThrottleEpoch) >= peer -> packetThrottleInterval)
    {
        peer -> lastRoundTripTime = peer -> lowestRoundTripTime;
        peer -> lastRoundTripTimeVariance = ENET_MAX (peer -> highestRoundTripTimeVariance, 1);
        peer -> lowestRoundTripTime = peer -> roundTripTime;
        peer -> highestRoundTripTimeVariance = peer -> roundTripTimeVariance;
        peer -> packetThrottleEpoch = host -> serviceTime;
    }

    peer -> lastReceiveTime = ENET_MAX (host -> serviceTime, 1);
    peer -> earliestTimeout = 0;

    receivedReliableSequenceNumber = ENET_NET_TO_HOST_16 (command -> acknowledge.receivedReliableSequenceNumber);

    commandNumber = enet_protocol_remove_sent_reliable_command (peer, receivedReliableSequenceNumber, command -> header.channelID);

    switch (peer -> state)
    {
    case ENET_PEER_STATE_ACKNOWLEDGING_CONNECT:
       if (commandNumber != ENET_PROTOCOL_COMMAND_VERIFY_CONNECT)
         return -1;

       enet_protocol_notify_connect (host, peer, event);
       break;

    case ENET_PEER_STATE_DISCONNECTING:
       if (commandNumber != ENET_PROTOCOL_COMMAND_DISCONNECT)
         return -1;

       enet_protocol_notify_disconnect (host, peer, event);
       break;

    case ENET_PEER_STATE_DISCONNECT_LATER:
       if (enet_list_empty (& peer -> outgoingCommands) &&
           enet_list_empty (& peer -> sentReliableCommands))
         enet_peer_disconnect (peer, peer -> eventData);
       break;

    default:
       break;
    }
   
    return 0;
}

static int
enet_protocol_handle_verify_connect (ENetHost * host, ENetEvent * event, ENetPeer * peer, const ENetProtocol * command)
{
    enet_uint32 mtu, windowSize;
    size_t channelCount;

    if (peer -> state != ENET_PEER_STATE_CONNECTING)
      return 0;

    channelCount = ENET_NET_TO_HOST_32 (command -> verifyConnect.channelCount);

    if (channelCount < ENET_PROTOCOL_MINIMUM_CHANNEL_COUNT || channelCount > ENET_PROTOCOL_MAXIMUM_CHANNEL_COUNT ||
        ENET_NET_TO_HOST_32 (command -> verifyConnect.packetThrottleInterval) != peer -> packetThrottleInterval ||
        ENET_NET_TO_HOST_32 (command -> verifyConnect.packetThrottleAcceleration) != peer -> packetThrottleAcceleration ||
        ENET_NET_TO_HOST_32 (command -> verifyConnect.packetThrottleDeceleration) != peer -> packetThrottleDeceleration ||
        command -> verifyConnect.connectID != peer -> connectID)
    {
        peer -> eventData = 0;

        enet_protocol_dispatch_state (host, peer, ENET_PEER_STATE_ZOMBIE);

        return -1;
    }

    enet_protocol_remove_sent_reliable_command (peer, 1, 0xFF);
    
    if (channelCount < peer -> channelCount)
      peer -> channelCount = channelCount;

    peer -> outgoingPeerID = ENET_NET_TO_HOST_16 (command -> verifyConnect.outgoingPeerID);
    peer -> incomingSessionID = command -> verifyConnect.incomingSessionID;
    peer -> outgoingSessionID = command -> verifyConnect.outgoingSessionID;

    mtu = ENET_NET_TO_HOST_32 (command -> verifyConnect.mtu);

    if (mtu < ENET_PROTOCOL_MINIMUM_MTU)
      mtu = ENET_PROTOCOL_MINIMUM_MTU;
    else 
    if (mtu > ENET_PROTOCOL_MAXIMUM_MTU)
      mtu = ENET_PROTOCOL_MAXIMUM_MTU;

    if (mtu < peer -> mtu)
      peer -> mtu = mtu;

    windowSize = ENET_NET_TO_HOST_32 (command -> verifyConnect.windowSize);

    if (windowSize < ENET_PROTOCOL_MINIMUM_WINDOW_SIZE)
      windowSize = ENET_PROTOCOL_MINIMUM_WINDOW_SIZE;

    if (windowSize > ENET_PROTOCOL_MAXIMUM_WINDOW_SIZE)
      windowSize = ENET_PROTOCOL_MAXIMUM_WINDOW_SIZE;

    if (windowSize < peer -> windowSize)
      peer -> windowSize = windowSize;

    peer -> incomingBandwidth = ENET_NET_TO_HOST_32 (command -> verifyConnect.incomingBandwidth);
    peer -> outgoingBandwidth = ENET_NET_TO_HOST_32 (command -> verifyConnect.outgoingBandwidth);

    enet_protocol_notify_connect (host, peer, event);
    return 0;
}

static int
enet_protocol_handle_incoming_commands (ENetHost * host, ENetEvent * event)
{
    ENetProtocolHeader * header;
    ENetProtocol * command;
    ENetPeer * peer;
    enet_uint8 * currentData;
    size_t headerSize;
    enet_uint16 peerID, flags;
    enet_uint8 sessionID;

    if (host -> receivedDataLength < (size_t) & ((ENetProtocolHeader *) 0) -> sentTime)
      return 0;

    header = (ENetProtocolHeader *) host -> receivedData;

    peerID = ENET_NET_TO_HOST_16 (header -> peerID);
    sessionID = (peerID & ENET_PROTOCOL_HEADER_SESSION_MASK) >> ENET_PROTOCOL_HEADER_SESSION_SHIFT;
    flags = peerID & ENET_PROTOCOL_HEADER_FLAG_MASK;
    peerID &= ~ (ENET_PROTOCOL_HEADER_FLAG_MASK | ENET_PROTOCOL_HEADER_SESSION_MASK);

    headerSize = (flags & ENET_PROTOCOL_HEADER_FLAG_SENT_TIME ? sizeof (ENetProtocolHeader) : (size_t) & ((ENetProtocolHeader *) 0) -> sentTime);
    if (host -> checksum != NULL)
      headerSize += sizeof (enet_uint32);

    if (peerID == ENET_PROTOCOL_MAXIMUM_PEER_ID)
      peer = NULL;
    else
    if (peerID >= host -> peerCount)
      return 0;
    else
    {
       peer = & host -> peers [peerID];

       if (peer -> state == ENET_PEER_STATE_DISCONNECTED ||
           peer -> state == ENET_PEER_STATE_ZOMBIE ||
           ((host -> receivedAddress.host != peer -> address.host ||
             host -> receivedAddress.port != peer -> address.port) &&
             peer -> address.host != ENET_HOST_BROADCAST) ||
           (peer -> outgoingPeerID < ENET_PROTOCOL_MAXIMUM_PEER_ID &&
            sessionID != peer -> incomingSessionID))
         return 0;
    }
 
    if (flags & ENET_PROTOCOL_HEADER_FLAG_COMPRESSED)
    {
        size_t originalSize;
        if (host -> compressor.context == NULL || host -> compressor.decompress == NULL)
          return 0;

        originalSize = host -> compressor.decompress (host -> compressor.context,
                                    host -> receivedData + headerSize, 
                                    host -> receivedDataLength - headerSize, 
                                    host -> packetData [1] + headerSize, 
                                    sizeof (host -> packetData [1]) - headerSize);
        if (originalSize <= 0 || originalSize > sizeof (host -> packetData [1]) - headerSize)
          return 0;

        memcpy (host -> packetData [1], header, headerSize);
        host -> receivedData = host -> packetData [1];
        host -> receivedDataLength = headerSize + originalSize;
    }

    if (host -> checksum != NULL)
    {
        enet_uint32 * checksum = (enet_uint32 *) & host -> receivedData [headerSize - sizeof (enet_uint32)],
                    desiredChecksum = * checksum;
        ENetBuffer buffer;

        * checksum = peer != NULL ? peer -> connectID : 0;

        buffer.data = host -> receivedData;
        buffer.dataLength = host -> receivedDataLength;

        if (host -> checksum (& buffer, 1) != desiredChecksum)
          return 0;
    }
       
    if (peer != NULL)
    {
       peer -> address.host = host -> receivedAddress.host;
       peer -> address.port = host -> receivedAddress.port;
       peer -> incomingDataTotal += host -> receivedDataLength;
    }
    
    currentData = host -> receivedData + headerSize;
  
    while (currentData < & host -> receivedData [host -> receivedDataLength])
    {
       enet_uint8 commandNumber;
       size_t commandSize;

       command = (ENetProtocol *) currentData;

       if (currentData + sizeof (ENetProtocolCommandHeader) > & host -> receivedData [host -> receivedDataLength])
         break;

       commandNumber = command -> header.command & ENET_PROTOCOL_COMMAND_MASK;
       if (commandNumber >= ENET_PROTOCOL_COMMAND_COUNT) 
         break;
       
       commandSize = commandSizes [commandNumber];
       if (commandSize == 0 || currentData + commandSize > & host -> receivedData [host -> receivedDataLength])
         break;

       currentData += commandSize;

       if (peer == NULL && commandNumber != ENET_PROTOCOL_COMMAND_CONNECT)
         break;
         
       command -> header.reliableSequenceNumber = ENET_NET_TO_HOST_16 (command -> header.reliableSequenceNumber);

       switch (commandNumber)
       {
       case ENET_PROTOCOL_COMMAND_ACKNOWLEDGE:
          if (enet_protocol_handle_acknowledge (host, event, peer, command))
            goto commandError;
          break;

       case ENET_PROTOCOL_COMMAND_CONNECT:
          if (peer != NULL)
            goto commandError;
          peer = enet_protocol_handle_connect (host, header, command);
          if (peer == NULL)
            goto commandError;
          break;

       case ENET_PROTOCOL_COMMAND_VERIFY_CONNECT:
          if (enet_protocol_handle_verify_connect (host, event, peer, command))
            goto commandError;
          break;

       case ENET_PROTOCOL_COMMAND_DISCONNECT:
          if (enet_protocol_handle_disconnect (host, peer, command))
            goto commandError;
          break;

       case ENET_PROTOCOL_COMMAND_PING:
          if (enet_protocol_handle_ping (host, peer, command))
            goto commandError;
          break;

       case ENET_PROTOCOL_COMMAND_SEND_RELIABLE:
          if (enet_protocol_handle_send_reliable (host, peer, command, & currentData))
            goto commandError;
          break;

       case ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE:
          if (enet_protocol_handle_send_unreliable (host, peer, command, & currentData))
            goto commandError;
          break;

       case ENET_PROTOCOL_COMMAND_SEND_UNSEQUENCED:
          if (enet_protocol_handle_send_unsequenced (host, peer, command, & currentData))
            goto commandError;
          break;

       case ENET_PROTOCOL_COMMAND_SEND_FRAGMENT:
          if (enet_protocol_handle_send_fragment (host, peer, command, & currentData))
            goto commandError;
          break;

       case ENET_PROTOCOL_COMMAND_BANDWIDTH_LIMIT:
          if (enet_protocol_handle_bandwidth_limit (host, peer, command))
            goto commandError;
          break;

       case ENET_PROTOCOL_COMMAND_THROTTLE_CONFIGURE:
          if (enet_protocol_handle_throttle_configure (host, peer, command))
            goto commandError;
          break;

       case ENET_PROTOCOL_COMMAND_SEND_UNRELIABLE_FRAGMENT:
          if (enet_protocol_handle_send_unreliable_fragment (host, peer, command, & currentData))
            goto commandError;
          break;

       default:
          goto commandError;
       }

       if (peer != NULL &&
           (command -> header.command & ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE) != 0)
       {
           enet_uint16 sentTime;

           if (! (flags & ENET_PROTOCOL_HEADER_FLAG_SENT_TIME))
             break;

           sentTime = ENET_NET_TO_HOST_16 (header -> sentTime);

           switch (peer -> state)
           {
           case ENET_PEER_STATE_DISCONNECTING:
           case ENET_PEER_STATE_ACKNOWLEDGING_CONNECT:
           case ENET_PEER_STATE_DISCONNECTED:
           case ENET_PEER_STATE_ZOMBIE:
              break;

           case ENET_PEER_STATE_ACKNOWLEDGING_DISCONNECT:
              if ((command -> header.command & ENET_PROTOCOL_COMMAND_MASK) == ENET_PROTOCOL_COMMAND_DISCONNECT)
                enet_peer_queue_acknowledgement (peer, command, sentTime);
              break;

           default:   
              enet_peer_queue_acknowledgement (peer, command, sentTime);        
              break;
           }
       }
    }

commandError:
    if (event != NULL && event -> type != ENET_EVENT_TYPE_NONE)
      return 1;

    return 0;
}
 
static int
enet_protocol_receive_incoming_commands (ENetHost * host, ENetEvent * event)
{
    int packets;

    for (packets = 0; packets < 256; ++ packets)
    {
       int receivedLength;
       ENetBuffer buffer;

       buffer.data = host -> packetData [0];
       buffer.dataLength = sizeof (host -> packetData [0]);

       receivedLength = enet_socket_receive (host -> socket,
                                             & host -> receivedAddress,
                                             & buffer,
                                             1);

       if (receivedLength < 0)
         return -1;

       if (receivedLength == 0)
         return 0;

       host -> receivedData = host -> packetData [0];
       host -> receivedDataLength = receivedLength;
      
       host -> totalReceivedData += receivedLength;
       host -> totalReceivedPackets ++;

       if (host -> intercept != NULL)
       {
          switch (host -> intercept (host, event))
          {
          case 1:
             if (event != NULL && event -> type != ENET_EVENT_TYPE_NONE)
               return 1;

             continue;
          
          case -1:
             return -1;
        
          default:
             break;
          }
       }
        
       switch (enet_protocol_handle_incoming_commands (host, event))
       {
       case 1:
          return 1;
       
       case -1:
          return -1;

       default:
          break;
       }
    }

    return 0;
}

static void
enet_protocol_send_acknowledgements (ENetHost * host, ENetPeer * peer)
{
    ENetProtocol * command = & host -> commands [host -> commandCount];
    ENetBuffer * buffer = & host -> buffers [host -> bufferCount];
    ENetAcknowledgement * acknowledgement;
    ENetListIterator currentAcknowledgement;
    enet_uint16 reliableSequenceNumber;
 
    currentAcknowledgement = enet_list_begin (& peer -> acknowledgements);
         
    while (currentAcknowledgement != enet_list_end (& peer -> acknowledgements))
    {
       if (command >= & host -> commands [sizeof (host -> commands) / sizeof (ENetProtocol)] ||
           buffer >= & host -> buffers [sizeof (host -> buffers) / sizeof (ENetBuffer)] ||
           peer -> mtu - host -> packetSize < sizeof (ENetProtocolAcknowledge))
       {
          host -> continueSending = 1;

          break;
       }

       acknowledgement = (ENetAcknowledgement *) currentAcknowledgement;
 
       currentAcknowledgement = enet_list_next (currentAcknowledgement);

       buffer -> data = command;
       buffer -> dataLength = sizeof (ENetProtocolAcknowledge);

       host -> packetSize += buffer -> dataLength;

       reliableSequenceNumber = ENET_HOST_TO_NET_16 (acknowledgement -> command.header.reliableSequenceNumber);
  
       command -> header.command = ENET_PROTOCOL_COMMAND_ACKNOWLEDGE;
       command -> header.channelID = acknowledgement -> command.header.channelID;
       command -> header.reliableSequenceNumber = reliableSequenceNumber;
       command -> acknowledge.receivedReliableSequenceNumber = reliableSequenceNumber;
       command -> acknowledge.receivedSentTime = ENET_HOST_TO_NET_16 (acknowledgement -> sentTime);
  
       if ((acknowledgement -> command.header.command & ENET_PROTOCOL_COMMAND_MASK) == ENET_PROTOCOL_COMMAND_DISCONNECT)
         enet_protocol_dispatch_state (host, peer, ENET_PEER_STATE_ZOMBIE);

       enet_list_remove (& acknowledgement -> acknowledgementList);
       enet_free (acknowledgement);

       ++ command;
       ++ buffer;
    }

    host -> commandCount = command - host -> commands;
    host -> bufferCount = buffer - host -> buffers;
}

static int
enet_protocol_check_timeouts (ENetHost * host, ENetPeer * peer, ENetEvent * event)
{
    ENetOutgoingCommand * outgoingCommand;
    ENetListIterator currentCommand, insertPosition;

    currentCommand = enet_list_begin (& peer -> sentReliableCommands);
    insertPosition = enet_list_begin (& peer -> outgoingCommands);

    while (currentCommand != enet_list_end (& peer -> sentReliableCommands))
    {
       outgoingCommand = (ENetOutgoingCommand *) currentCommand;

       currentCommand = enet_list_next (currentCommand);

       if (ENET_TIME_DIFFERENCE (host -> serviceTime, outgoingCommand -> sentTime) < outgoingCommand -> roundTripTimeout)
         continue;

       if (peer -> earliestTimeout == 0 ||
           ENET_TIME_LESS (outgoingCommand -> sentTime, peer -> earliestTimeout))
         peer -> earliestTimeout = outgoingCommand -> sentTime;

       if (peer -> earliestTimeout != 0 &&
             (ENET_TIME_DIFFERENCE (host -> serviceTime, peer -> earliestTimeout) >= peer -> timeoutMaximum ||
               (outgoingCommand -> roundTripTimeout >= outgoingCommand -> roundTripTimeoutLimit &&
                 ENET_TIME_DIFFERENCE (host -> serviceTime, peer -> earliestTimeout) >= peer -> timeoutMinimum)))
       {
          enet_protocol_notify_disconnect (host, peer, event);

          return 1;
       }

       if (outgoingCommand -> packet != NULL)
         peer -> reliableDataInTransit -= outgoingCommand -> fragmentLength;
          
       ++ peer -> packetsLost;

       outgoingCommand -> roundTripTimeout *= 2;

       enet_list_insert (insertPosition, enet_list_remove (& outgoingCommand -> outgoingCommandList));

       if (currentCommand == enet_list_begin (& peer -> sentReliableCommands) &&
           ! enet_list_empty (& peer -> sentReliableCommands))
       {
          outgoingCommand = (ENetOutgoingCommand *) currentCommand;

          peer -> nextTimeout = outgoingCommand -> sentTime + outgoingCommand -> roundTripTimeout;
       }
    }
    
    return 0;
}

static int
enet_protocol_check_outgoing_commands (ENetHost * host, ENetPeer * peer)
{
    ENetProtocol * command = & host -> commands [host -> commandCount];
    ENetBuffer * buffer = & host -> buffers [host -> bufferCount];
    ENetOutgoingCommand * outgoingCommand;
    ENetListIterator currentCommand;
    ENetChannel *channel = NULL;
    enet_uint16 reliableWindow = 0;
    size_t commandSize;
    int windowExceeded = 0, windowWrap = 0, canPing = 1;

    currentCommand = enet_list_begin (& peer -> outgoingCommands);
    
    while (currentCommand != enet_list_end (& peer -> outgoingCommands))
    {
       outgoingCommand = (ENetOutgoingCommand *) currentCommand;

       if (outgoingCommand -> command.header.command & ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE)
       {
          channel = outgoingCommand -> command.header.channelID < peer -> channelCount ? & peer -> channels [outgoingCommand -> command.header.channelID] : NULL;
          reliableWindow = outgoingCommand -> reliableSequenceNumber / ENET_PEER_RELIABLE_WINDOW_SIZE;
          if (channel != NULL)
          {
             if (! windowWrap &&      
                  outgoingCommand -> sendAttempts < 1 && 
                  ! (outgoingCommand -> reliableSequenceNumber % ENET_PEER_RELIABLE_WINDOW_SIZE) &&
                  (channel -> reliableWindows [(reliableWindow + ENET_PEER_RELIABLE_WINDOWS - 1) % ENET_PEER_RELIABLE_WINDOWS] >= ENET_PEER_RELIABLE_WINDOW_SIZE ||
                    channel -> usedReliableWindows & ((((1 << (ENET_PEER_FREE_RELIABLE_WINDOWS + 2)) - 1) << reliableWindow) |
                      (((1 << (ENET_PEER_FREE_RELIABLE_WINDOWS + 2)) - 1) >> (ENET_PEER_RELIABLE_WINDOWS - reliableWindow)))))
                windowWrap = 1;
             if (windowWrap)
             {
                currentCommand = enet_list_next (currentCommand);
 
                continue;
             }
          }
 
          if (outgoingCommand -> packet != NULL)
          {
             if (! windowExceeded)
             {
                enet_uint32 windowSize = (peer -> packetThrottle * peer -> windowSize) / ENET_PEER_PACKET_THROTTLE_SCALE;
             
                if (peer -> reliableDataInTransit + outgoingCommand -> fragmentLength > ENET_MAX (windowSize, peer -> mtu))
                  windowExceeded = 1;
             }
             if (windowExceeded)
             {
                currentCommand = enet_list_next (currentCommand);

                continue;
             }
          }

          canPing = 0;
       }

       commandSize = commandSizes [outgoingCommand -> command.header.command & ENET_PROTOCOL_COMMAND_MASK];
       if (command >= & host -> commands [sizeof (host -> commands) / sizeof (ENetProtocol)] ||
           buffer + 1 >= & host -> buffers [sizeof (host -> buffers) / sizeof (ENetBuffer)] ||
           peer -> mtu - host -> packetSize < commandSize ||
           (outgoingCommand -> packet != NULL && 
             (enet_uint16) (peer -> mtu - host -> packetSize) < (enet_uint16) (commandSize + outgoingCommand -> fragmentLength)))
       {
          host -> continueSending = 1;
          
          break;
       }

       currentCommand = enet_list_next (currentCommand);

       if (outgoingCommand -> command.header.command & ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE)
       {
          if (channel != NULL && outgoingCommand -> sendAttempts < 1)
          {
             channel -> usedReliableWindows |= 1 << reliableWindow;
             ++ channel -> reliableWindows [reliableWindow];
          }

          ++ outgoingCommand -> sendAttempts;
 
          if (outgoingCommand -> roundTripTimeout == 0)
          {
             outgoingCommand -> roundTripTimeout = peer -> roundTripTime + 4 * peer -> roundTripTimeVariance;
             outgoingCommand -> roundTripTimeoutLimit = peer -> timeoutLimit * outgoingCommand -> roundTripTimeout;
          }

          if (enet_list_empty (& peer -> sentReliableCommands))
            peer -> nextTimeout = host -> serviceTime + outgoingCommand -> roundTripTimeout;

          enet_list_insert (enet_list_end (& peer -> sentReliableCommands),
                            enet_list_remove (& outgoingCommand -> outgoingCommandList));

          outgoingCommand -> sentTime = host -> serviceTime;

          host -> headerFlags |= ENET_PROTOCOL_HEADER_FLAG_SENT_TIME;

          peer -> reliableDataInTransit += outgoingCommand -> fragmentLength;
       }
       else
       {
          if (outgoingCommand -> packet != NULL && outgoingCommand -> fragmentOffset == 0)
          {
             peer -> packetThrottleCounter += ENET_PEER_PACKET_THROTTLE_COUNTER;
             peer -> packetThrottleCounter %= ENET_PEER_PACKET_THROTTLE_SCALE;

             if (peer -> packetThrottleCounter > peer -> packetThrottle)
             {
                enet_uint16 reliableSequenceNumber = outgoingCommand -> reliableSequenceNumber,
                            unreliableSequenceNumber = outgoingCommand -> unreliableSequenceNumber;
                for (;;)
                {
                   -- outgoingCommand -> packet -> referenceCount;

                   if (outgoingCommand -> packet -> referenceCount == 0)
                     enet_packet_destroy (outgoingCommand -> packet);

                   enet_list_remove (& outgoingCommand -> outgoingCommandList);
                   enet_free (outgoingCommand);

                   if (currentCommand == enet_list_end (& peer -> outgoingCommands))
                     break;

                   outgoingCommand = (ENetOutgoingCommand *) currentCommand;
                   if (outgoingCommand -> reliableSequenceNumber != reliableSequenceNumber ||
                       outgoingCommand -> unreliableSequenceNumber != unreliableSequenceNumber)
                     break;

                   currentCommand = enet_list_next (currentCommand);
                }

                continue;
             }
          }

          enet_list_remove (& outgoingCommand -> outgoingCommandList);

          if (outgoingCommand -> packet != NULL)
            enet_list_insert (enet_list_end (& peer -> sentUnreliableCommands), outgoingCommand);
       }

       buffer -> data = command;
       buffer -> dataLength = commandSize;

       host -> packetSize += buffer -> dataLength;

       * command = outgoingCommand -> command;

       if (outgoingCommand -> packet != NULL)
       {
          ++ buffer;
          
          buffer -> data = outgoingCommand -> packet -> data + outgoingCommand -> fragmentOffset;
          buffer -> dataLength = outgoingCommand -> fragmentLength;

          host -> packetSize += outgoingCommand -> fragmentLength;
       }
       else
       if (! (outgoingCommand -> command.header.command & ENET_PROTOCOL_COMMAND_FLAG_ACKNOWLEDGE))
         enet_free (outgoingCommand);

       ++ peer -> packetsSent;
        
       ++ command;
       ++ buffer;
    }

    host -> commandCount = command - host -> commands;
    host -> bufferCount = buffer - host -> buffers;

    if (peer -> state == ENET_PEER_STATE_DISCONNECT_LATER &&
        enet_list_empty (& peer -> outgoingCommands) &&
        enet_list_empty (& peer -> sentReliableCommands) &&
        enet_list_empty (& peer -> sentUnreliableCommands))
      enet_peer_disconnect (peer, peer -> eventData);

    return canPing;
}

static int
enet_protocol_send_outgoing_commands (ENetHost * host, ENetEvent * event, int checkForTimeouts)
{
    enet_uint8 headerData [sizeof (ENetProtocolHeader) + sizeof (enet_uint32)];
    ENetProtocolHeader * header = (ENetProtocolHeader *) headerData;
    ENetPeer * currentPeer;
    int sentLength;
    size_t shouldCompress = 0;
 
    host -> continueSending = 1;

    while (host -> continueSending)
    for (host -> continueSending = 0,
           currentPeer = host -> peers;
         currentPeer < & host -> peers [host -> peerCount];
         ++ currentPeer)
    {
        if (currentPeer -> state == ENET_PEER_STATE_DISCONNECTED ||
            currentPeer -> state == ENET_PEER_STATE_ZOMBIE)
          continue;

        host -> headerFlags = 0;
        host -> commandCount = 0;
        host -> bufferCount = 1;
        host -> packetSize = sizeof (ENetProtocolHeader);

        if (! enet_list_empty (& currentPeer -> acknowledgements))
          enet_protocol_send_acknowledgements (host, currentPeer);

        if (checkForTimeouts != 0 &&
            ! enet_list_empty (& currentPeer -> sentReliableCommands) &&
            ENET_TIME_GREATER_EQUAL (host -> serviceTime, currentPeer -> nextTimeout) &&
            enet_protocol_check_timeouts (host, currentPeer, event) == 1)
        {
            if (event != NULL && event -> type != ENET_EVENT_TYPE_NONE)
              return 1;
            else
              continue;
        }

        if ((enet_list_empty (& currentPeer -> outgoingCommands) ||
              enet_protocol_check_outgoing_commands (host, currentPeer)) &&
            enet_list_empty (& currentPeer -> sentReliableCommands) &&
            ENET_TIME_DIFFERENCE (host -> serviceTime, currentPeer -> lastReceiveTime) >= currentPeer -> pingInterval &&
            currentPeer -> mtu - host -> packetSize >= sizeof (ENetProtocolPing))
        { 
            enet_peer_ping (currentPeer);
            enet_protocol_check_outgoing_commands (host, currentPeer);
        }

        if (host -> commandCount == 0)
          continue;

        if (currentPeer -> packetLossEpoch == 0)
          currentPeer -> packetLossEpoch = host -> serviceTime;
        else
        if (ENET_TIME_DIFFERENCE (host -> serviceTime, currentPeer -> packetLossEpoch) >= ENET_PEER_PACKET_LOSS_INTERVAL &&
            currentPeer -> packetsSent > 0)
        {
           enet_uint32 packetLoss = currentPeer -> packetsLost * ENET_PEER_PACKET_LOSS_SCALE / currentPeer -> packetsSent;

#ifdef ENET_DEBUG
           printf ("peer %u: %f%%+-%f%% packet loss, %u+-%u ms round trip time, %f%% throttle, %u outgoing, %u/%u incoming\n", currentPeer -> incomingPeerID, currentPeer -> packetLoss / (float) ENET_PEER_PACKET_LOSS_SCALE, currentPeer -> packetLossVariance / (float) ENET_PEER_PACKET_LOSS_SCALE, currentPeer -> roundTripTime, currentPeer -> roundTripTimeVariance, currentPeer -> packetThrottle / (float) ENET_PEER_PACKET_THROTTLE_SCALE, enet_list_size (& currentPeer -> outgoingCommands), currentPeer -> channels != NULL ? enet_list_size (& currentPeer -> channels -> incomingReliableCommands) : 0, currentPeer -> channels != NULL ? enet_list_size (& currentPeer -> channels -> incomingUnreliableCommands) : 0);
#endif

           currentPeer -> packetLossVariance = (currentPeer -> packetLossVariance * 3 + ENET_DIFFERENCE (packetLoss, currentPeer -> packetLoss)) / 4;
           currentPeer -> packetLoss = (currentPeer -> packetLoss * 7 + packetLoss) / 8;

           currentPeer -> packetLossEpoch = host -> serviceTime;
           currentPeer -> packetsSent = 0;
           currentPeer -> packetsLost = 0;
        }

        host -> buffers -> data = headerData;
        if (host -> headerFlags & ENET_PROTOCOL_HEADER_FLAG_SENT_TIME)
        {
            header -> sentTime = ENET_HOST_TO_NET_16 (host -> serviceTime & 0xFFFF);

            host -> buffers -> dataLength = sizeof (ENetProtocolHeader);
        }
        else
          host -> buffers -> dataLength = (size_t) & ((ENetProtocolHeader *) 0) -> sentTime;

        shouldCompress = 0;
        if (host -> compressor.context != NULL && host -> compressor.compress != NULL)
        {
            size_t originalSize = host -> packetSize - sizeof(ENetProtocolHeader),
                   compressedSize = host -> compressor.compress (host -> compressor.context,
                                        & host -> buffers [1], host -> bufferCount - 1,
                                        originalSize,
                                        host -> packetData [1],
                                        originalSize);
            if (compressedSize > 0 && compressedSize < originalSize)
            {
                host -> headerFlags |= ENET_PROTOCOL_HEADER_FLAG_COMPRESSED;
                shouldCompress = compressedSize;
#ifdef ENET_DEBUG_COMPRESS
                printf ("peer %u: compressed %u -> %u (%u%%)\n", currentPeer -> incomingPeerID, originalSize, compressedSize, (compressedSize * 100) / originalSize);
#endif
            }
        }

        if (currentPeer -> outgoingPeerID < ENET_PROTOCOL_MAXIMUM_PEER_ID)
          host -> headerFlags |= currentPeer -> outgoingSessionID << ENET_PROTOCOL_HEADER_SESSION_SHIFT;
        header -> peerID = ENET_HOST_TO_NET_16 (currentPeer -> outgoingPeerID | host -> headerFlags);
        if (host -> checksum != NULL)
        {
            enet_uint32 * checksum = (enet_uint32 *) & headerData [host -> buffers -> dataLength];
            * checksum = currentPeer -> outgoingPeerID < ENET_PROTOCOL_MAXIMUM_PEER_ID ? currentPeer -> connectID : 0;
            host -> buffers -> dataLength += sizeof (enet_uint32);
            * checksum = host -> checksum (host -> buffers, host -> bufferCount);
        }

        if (shouldCompress > 0)
        {
            host -> buffers [1].data = host -> packetData [1];
            host -> buffers [1].dataLength = shouldCompress;
            host -> bufferCount = 2;
        }

        currentPeer -> lastSendTime = host -> serviceTime;

        sentLength = enet_socket_send (host -> socket, & currentPeer -> address, host -> buffers, host -> bufferCount);

        enet_protocol_remove_sent_unreliable_commands (currentPeer);

        if (sentLength < 0)
          return -1;

        host -> totalSentData += sentLength;
        host -> totalSentPackets ++;
    }
   
    return 0;
}

/** Sends any queued packets on the host specified to its designated peers.

    @param host   host to flush
    @remarks this function need only be used in circumstances where one wishes to send queued packets earlier than in a call to enet_host_service().
    @ingroup host
*/
void
enet_host_flush (ENetHost * host)
{
    host -> serviceTime = enet_time_get ();

    enet_protocol_send_outgoing_commands (host, NULL, 0);
}

/** Checks for any queued events on the host and dispatches one if available.

    @param host    host to check for events
    @param event   an event structure where event details will be placed if available
    @retval > 0 if an event was dispatched
    @retval 0 if no events are available
    @retval < 0 on failure
    @ingroup host
*/
int
enet_host_check_events (ENetHost * host, ENetEvent * event)
{
    if (event == NULL) return -1;

    event -> type = ENET_EVENT_TYPE_NONE;
    event -> peer = NULL;
    event -> packet = NULL;

    return enet_protocol_dispatch_incoming_commands (host, event);
}

/** Waits for events on the host specified and shuttles packets between
    the host and its peers.

    @param host    host to service
    @param event   an event structure where event details will be placed if one occurs
                   if event == NULL then no events will be delivered
    @param timeout number of milliseconds that ENet should wait for events
    @retval > 0 if an event occurred within the specified time limit
    @retval 0 if no event occurred
    @retval < 0 on failure
    @remarks enet_host_service should be called fairly regularly for adequate performance
    @ingroup host
*/
int
enet_host_service (ENetHost * host, ENetEvent * event, enet_uint32 timeout)
{
    enet_uint32 waitCondition;

    if (event != NULL)
    {
        event -> type = ENET_EVENT_TYPE_NONE;
        event -> peer = NULL;
        event -> packet = NULL;

        switch (enet_protocol_dispatch_incoming_commands (host, event))
        {
        case 1:
            return 1;

        case -1:
#ifdef ENET_DEBUG
            perror ("Error dispatching incoming packets");
#endif

            return -1;

        default:
            break;
        }
    }

    host -> serviceTime = enet_time_get ();
    
    timeout += host -> serviceTime;

    do
    {
       if (ENET_TIME_DIFFERENCE (host -> serviceTime, host -> bandwidthThrottleEpoch) >= ENET_HOST_BANDWIDTH_THROTTLE_INTERVAL)
         enet_host_bandwidth_throttle (host);

       switch (enet_protocol_send_outgoing_commands (host, event, 1))
       {
       case 1:
          return 1;

       case -1:
#ifdef ENET_DEBUG
          perror ("Error sending outgoing packets");
#endif

          return -1;

       default:
          break;
       }

       switch (enet_protocol_receive_incoming_commands (host, event))
       {
       case 1:
          return 1;

       case -1:
#ifdef ENET_DEBUG
          perror ("Error receiving incoming packets");
#endif

          return -1;

       default:
          break;
       }

       switch (enet_protocol_send_outgoing_commands (host, event, 1))
       {
       case 1:
          return 1;

       case -1:
#ifdef ENET_DEBUG
          perror ("Error sending outgoing packets");
#endif

          return -1;

       default:
          break;
       }

       if (event != NULL)
       {
          switch (enet_protocol_dispatch_incoming_commands (host, event))
          {
          case 1:
             return 1;

          case -1:
#ifdef ENET_DEBUG
             perror ("Error dispatching incoming packets");
#endif

             return -1;

          default:
             break;
          }
       }

       if (ENET_TIME_GREATER_EQUAL (host -> serviceTime, timeout))
         return 0;

       do
       {
          host -> serviceTime = enet_time_get ();

          if (ENET_TIME_GREATER_EQUAL (host -> serviceTime, timeout))
            return 0;

          waitCondition = ENET_SOCKET_WAIT_RECEIVE | ENET_SOCKET_WAIT_INTERRUPT;

          if (enet_socket_wait (host -> socket, & waitCondition, ENET_TIME_DIFFERENCE (timeout, host -> serviceTime)) != 0)
            return -1;
       }
       while (waitCondition & ENET_SOCKET_WAIT_INTERRUPT);

       host -> serviceTime = enet_time_get ();
    } while (waitCondition & ENET_SOCKET_WAIT_RECEIVE);

    return 0; 
}

// ----- unix.c -----

#ifndef _WIN32

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#ifdef __APPLE__
#ifdef HAS_POLL
#undef HAS_POLL
#endif
#ifndef HAS_FCNTL
#define HAS_FCNTL 1
#endif
#ifndef HAS_INET_PTON
#define HAS_INET_PTON 1
#endif
#ifndef HAS_INET_NTOP
#define HAS_INET_NTOP 1
#endif
#ifndef HAS_MSGHDR_FLAGS
#define HAS_MSGHDR_FLAGS 1
#endif
#ifndef HAS_SOCKLEN_T
#define HAS_SOCKLEN_T 1
#endif
#ifndef HAS_GETADDRINFO
#define HAS_GETADDRINFO 1
#endif
#ifndef HAS_GETNAMEINFO
#define HAS_GETNAMEINFO 1
#endif
#endif

#ifdef HAS_FCNTL
#include <fcntl.h>
#endif

#ifdef HAS_POLL
#include <poll.h>
#endif

#if !defined(HAS_SOCKLEN_T) && !defined(__socklen_t_defined)
typedef int socklen_t;
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static enet_uint32 timeBase = 0;

int
enet_initialize (void)
{
    return 0;
}

void
enet_deinitialize (void)
{
}

enet_uint32
enet_host_random_seed (void)
{
    return (enet_uint32) time (NULL);
}

enet_uint32
enet_time_get (void)
{
    struct timeval timeVal;

    gettimeofday (& timeVal, NULL);

    return timeVal.tv_sec * 1000 + timeVal.tv_usec / 1000 - timeBase;
}

void
enet_time_set (enet_uint32 newTimeBase)
{
    struct timeval timeVal;

    gettimeofday (& timeVal, NULL);
    
    timeBase = timeVal.tv_sec * 1000 + timeVal.tv_usec / 1000 - newTimeBase;
}

int
enet_address_set_host_ip (ENetAddress * address, const char * name)
{
#ifdef HAS_INET_PTON
    if (! inet_pton (AF_INET, name, & address -> host))
#else
    if (! inet_aton (name, (struct in_addr *) & address -> host))
#endif
        return -1;

    return 0;
}

int
enet_address_set_host (ENetAddress * address, const char * name)
{
#ifdef HAS_GETADDRINFO
    struct addrinfo hints, * resultList = NULL, * result = NULL;

    memset (& hints, 0, sizeof (hints));
    hints.ai_family = AF_INET;

    if (getaddrinfo (name, NULL, NULL, & resultList) != 0)
      return -1;

    for (result = resultList; result != NULL; result = result -> ai_next)
    {
        if (result -> ai_family == AF_INET && result -> ai_addr != NULL && result -> ai_addrlen >= sizeof (struct sockaddr_in))
        {
            struct sockaddr_in * sin = (struct sockaddr_in *) result -> ai_addr;

            address -> host = sin -> sin_addr.s_addr;

            freeaddrinfo (resultList);

            return 0;
        }
    }

    if (resultList != NULL)
      freeaddrinfo (resultList);
#else
    struct hostent * hostEntry = NULL;
#ifdef HAS_GETHOSTBYNAME_R
    struct hostent hostData;
    char buffer [2048];
    int errnum;

#if defined(linux) || defined(__linux) || defined(__linux__) || defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__DragonFly__) || defined(__GNU__)
    gethostbyname_r (name, & hostData, buffer, sizeof (buffer), & hostEntry, & errnum);
#else
    hostEntry = gethostbyname_r (name, & hostData, buffer, sizeof (buffer), & errnum);
#endif
#else
    hostEntry = gethostbyname (name);
#endif

    if (hostEntry != NULL && hostEntry -> h_addrtype == AF_INET)
    {
        address -> host = * (enet_uint32 *) hostEntry -> h_addr_list [0];

        return 0;
    }
#endif

    return enet_address_set_host_ip (address, name);
}

int
enet_address_get_host_ip (const ENetAddress * address, char * name, size_t nameLength)
{
#ifdef HAS_INET_NTOP
    if (inet_ntop (AF_INET, & address -> host, name, nameLength) == NULL)
#else
    char * addr = inet_ntoa (* (struct in_addr *) & address -> host);
    if (addr != NULL)
    {
        size_t addrLen = strlen(addr);
        if (addrLen >= nameLength)
          return -1;
        memcpy (name, addr, addrLen + 1);
    } 
    else
#endif
        return -1;
    return 0;
}

int
enet_address_get_host (const ENetAddress * address, char * name, size_t nameLength)
{
#ifdef HAS_GETNAMEINFO
    struct sockaddr_in sin;
    int err;

    memset (& sin, 0, sizeof (struct sockaddr_in));

    sin.sin_family = AF_INET;
    sin.sin_port = ENET_HOST_TO_NET_16 (address -> port);
    sin.sin_addr.s_addr = address -> host;

    err = getnameinfo ((struct sockaddr *) & sin, sizeof (sin), name, nameLength, NULL, 0, NI_NAMEREQD);
    if (! err)
    {
        if (name != NULL && nameLength > 0 && ! memchr (name, '\0', nameLength))
          return -1;
        return 0;
    }
    if (err != EAI_NONAME)
      return -1;
#else
    struct in_addr in;
    struct hostent * hostEntry = NULL;
#ifdef HAS_GETHOSTBYADDR_R
    struct hostent hostData;
    char buffer [2048];
    int errnum;

    in.s_addr = address -> host;

#if defined(linux) || defined(__linux) || defined(__linux__) || defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__DragonFly__) || defined(__GNU__)
    gethostbyaddr_r ((char *) & in, sizeof (struct in_addr), AF_INET, & hostData, buffer, sizeof (buffer), & hostEntry, & errnum);
#else
    hostEntry = gethostbyaddr_r ((char *) & in, sizeof (struct in_addr), AF_INET, & hostData, buffer, sizeof (buffer), & errnum);
#endif
#else
    in.s_addr = address -> host;

    hostEntry = gethostbyaddr ((char *) & in, sizeof (struct in_addr), AF_INET);
#endif

    if (hostEntry != NULL)
    {
       size_t hostLen = strlen (hostEntry -> h_name);
       if (hostLen >= nameLength)
         return -1;
       memcpy (name, hostEntry -> h_name, hostLen + 1);
       return 0;
    }
#endif

    return enet_address_get_host_ip (address, name, nameLength);
}

int
enet_socket_bind (ENetSocket socket, const ENetAddress * address)
{
    struct sockaddr_in sin;

    memset (& sin, 0, sizeof (struct sockaddr_in));

    sin.sin_family = AF_INET;

    if (address != NULL)
    {
       sin.sin_port = ENET_HOST_TO_NET_16 (address -> port);
       sin.sin_addr.s_addr = address -> host;
    }
    else
    {
       sin.sin_port = 0;
       sin.sin_addr.s_addr = INADDR_ANY;
    }

    return bind (socket,
                 (struct sockaddr *) & sin,
                 sizeof (struct sockaddr_in)); 
}

int
enet_socket_get_address (ENetSocket socket, ENetAddress * address)
{
    struct sockaddr_in sin;
    socklen_t sinLength = sizeof (struct sockaddr_in);

    if (getsockname (socket, (struct sockaddr *) & sin, & sinLength) == -1)
      return -1;

    address -> host = (enet_uint32) sin.sin_addr.s_addr;
    address -> port = ENET_NET_TO_HOST_16 (sin.sin_port);

    return 0;
}

int 
enet_socket_listen (ENetSocket socket, int backlog)
{
    return listen (socket, backlog < 0 ? SOMAXCONN : backlog);
}

ENetSocket
enet_socket_create (ENetSocketType type)
{
    return socket (PF_INET, type == ENET_SOCKET_TYPE_DATAGRAM ? SOCK_DGRAM : SOCK_STREAM, 0);
}

int
enet_socket_set_option (ENetSocket socket, ENetSocketOption option, int value)
{
    int result = -1;
    switch (option)
    {
        case ENET_SOCKOPT_NONBLOCK:
#ifdef HAS_FCNTL
            result = fcntl (socket, F_SETFL, (value ? O_NONBLOCK : 0) | (fcntl (socket, F_GETFL) & ~O_NONBLOCK));
#else
            result = ioctl (socket, FIONBIO, & value);
#endif
            break;

        case ENET_SOCKOPT_BROADCAST:
            result = setsockopt (socket, SOL_SOCKET, SO_BROADCAST, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_REUSEADDR:
            result = setsockopt (socket, SOL_SOCKET, SO_REUSEADDR, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_RCVBUF:
            result = setsockopt (socket, SOL_SOCKET, SO_RCVBUF, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_SNDBUF:
            result = setsockopt (socket, SOL_SOCKET, SO_SNDBUF, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_RCVTIMEO:
        {
            struct timeval timeVal;
            timeVal.tv_sec = value / 1000;
            timeVal.tv_usec = (value % 1000) * 1000;
            result = setsockopt (socket, SOL_SOCKET, SO_RCVTIMEO, (char *) & timeVal, sizeof (struct timeval));
            break;
        }

        case ENET_SOCKOPT_SNDTIMEO:
        {
            struct timeval timeVal;
            timeVal.tv_sec = value / 1000;
            timeVal.tv_usec = (value % 1000) * 1000;
            result = setsockopt (socket, SOL_SOCKET, SO_SNDTIMEO, (char *) & timeVal, sizeof (struct timeval));
            break;
        }

        case ENET_SOCKOPT_NODELAY:
            result = setsockopt (socket, IPPROTO_TCP, TCP_NODELAY, (char *) & value, sizeof (int));
            break;

        default:
            break;
    }
    return result == -1 ? -1 : 0;
}

int
enet_socket_get_option (ENetSocket socket, ENetSocketOption option, int * value)
{
    int result = -1;
    socklen_t len;
    switch (option)
    {
        case ENET_SOCKOPT_ERROR:
            len = sizeof (int);
            result = getsockopt (socket, SOL_SOCKET, SO_ERROR, value, & len);
            break;

        default:
            break;
    }
    return result == -1 ? -1 : 0;
}

int
enet_socket_connect (ENetSocket socket, const ENetAddress * address)
{
    struct sockaddr_in sin;
    int result;

    memset (& sin, 0, sizeof (struct sockaddr_in));

    sin.sin_family = AF_INET;
    sin.sin_port = ENET_HOST_TO_NET_16 (address -> port);
    sin.sin_addr.s_addr = address -> host;

    result = connect (socket, (struct sockaddr *) & sin, sizeof (struct sockaddr_in));
    if (result == -1 && errno == EINPROGRESS)
      return 0;

    return result;
}

ENetSocket
enet_socket_accept (ENetSocket socket, ENetAddress * address)
{
    int result;
    struct sockaddr_in sin;
    socklen_t sinLength = sizeof (struct sockaddr_in);

    result = accept (socket, 
                     address != NULL ? (struct sockaddr *) & sin : NULL, 
                     address != NULL ? & sinLength : NULL);
    
    if (result == -1)
      return ENET_SOCKET_NULL;

    if (address != NULL)
    {
        address -> host = (enet_uint32) sin.sin_addr.s_addr;
        address -> port = ENET_NET_TO_HOST_16 (sin.sin_port);
    }

    return result;
} 
    
int
enet_socket_shutdown (ENetSocket socket, ENetSocketShutdown how)
{
    return shutdown (socket, (int) how);
}

void
enet_socket_destroy (ENetSocket socket)
{
    if (socket != -1)
      close (socket);
}

int
enet_socket_send (ENetSocket socket,
                  const ENetAddress * address,
                  const ENetBuffer * buffers,
                  size_t bufferCount)
{
    struct msghdr msgHdr;
    struct sockaddr_in sin;
    int sentLength;

    memset (& msgHdr, 0, sizeof (struct msghdr));

    if (address != NULL)
    {
        memset (& sin, 0, sizeof (struct sockaddr_in));

        sin.sin_family = AF_INET;
        sin.sin_port = ENET_HOST_TO_NET_16 (address -> port);
        sin.sin_addr.s_addr = address -> host;

        msgHdr.msg_name = & sin;
        msgHdr.msg_namelen = sizeof (struct sockaddr_in);
    }

    msgHdr.msg_iov = (struct iovec *) buffers;
    msgHdr.msg_iovlen = bufferCount;

    sentLength = sendmsg (socket, & msgHdr, MSG_NOSIGNAL);
    
    if (sentLength == -1)
    {
       if (errno == EWOULDBLOCK)
         return 0;

       return -1;
    }

    return sentLength;
}

int
enet_socket_receive (ENetSocket socket,
                     ENetAddress * address,
                     ENetBuffer * buffers,
                     size_t bufferCount)
{
    struct msghdr msgHdr;
    struct sockaddr_in sin;
    int recvLength;

    memset (& msgHdr, 0, sizeof (struct msghdr));

    if (address != NULL)
    {
        msgHdr.msg_name = & sin;
        msgHdr.msg_namelen = sizeof (struct sockaddr_in);
    }

    msgHdr.msg_iov = (struct iovec *) buffers;
    msgHdr.msg_iovlen = bufferCount;

    recvLength = recvmsg (socket, & msgHdr, MSG_NOSIGNAL);

    if (recvLength == -1)
    {
       if (errno == EWOULDBLOCK)
         return 0;

       return -1;
    }

#ifdef HAS_MSGHDR_FLAGS
    if (msgHdr.msg_flags & MSG_TRUNC)
      return -1;
#endif

    if (address != NULL)
    {
        address -> host = (enet_uint32) sin.sin_addr.s_addr;
        address -> port = ENET_NET_TO_HOST_16 (sin.sin_port);
    }

    return recvLength;
}

int
enet_socketset_select (ENetSocket maxSocket, ENetSocketSet * readSet, ENetSocketSet * writeSet, enet_uint32 timeout)
{
    struct timeval timeVal;

    timeVal.tv_sec = timeout / 1000;
    timeVal.tv_usec = (timeout % 1000) * 1000;

    return select (maxSocket + 1, readSet, writeSet, NULL, & timeVal);
}

int
enet_socket_wait (ENetSocket socket, enet_uint32 * condition, enet_uint32 timeout)
{
#ifdef HAS_POLL
    struct pollfd pollSocket;
    int pollCount;
    
    pollSocket.fd = socket;
    pollSocket.events = 0;

    if (* condition & ENET_SOCKET_WAIT_SEND)
      pollSocket.events |= POLLOUT;

    if (* condition & ENET_SOCKET_WAIT_RECEIVE)
      pollSocket.events |= POLLIN;

    pollCount = poll (& pollSocket, 1, timeout);

    if (pollCount < 0)
    {
        if (errno == EINTR && * condition & ENET_SOCKET_WAIT_INTERRUPT)
        {
            * condition = ENET_SOCKET_WAIT_INTERRUPT;

            return 0;
        }

        return -1;
    }

    * condition = ENET_SOCKET_WAIT_NONE;

    if (pollCount == 0)
      return 0;

    if (pollSocket.revents & POLLOUT)
      * condition |= ENET_SOCKET_WAIT_SEND;
    
    if (pollSocket.revents & POLLIN)
      * condition |= ENET_SOCKET_WAIT_RECEIVE;

    return 0;
#else
    fd_set readSet, writeSet;
    struct timeval timeVal;
    int selectCount;

    timeVal.tv_sec = timeout / 1000;
    timeVal.tv_usec = (timeout % 1000) * 1000;

    FD_ZERO (& readSet);
    FD_ZERO (& writeSet);

    if (* condition & ENET_SOCKET_WAIT_SEND)
      FD_SET (socket, & writeSet);

    if (* condition & ENET_SOCKET_WAIT_RECEIVE)
      FD_SET (socket, & readSet);

    selectCount = select (socket + 1, & readSet, & writeSet, NULL, & timeVal);

    if (selectCount < 0)
    {
        if (errno == EINTR && * condition & ENET_SOCKET_WAIT_INTERRUPT)
        {
            * condition = ENET_SOCKET_WAIT_INTERRUPT;

            return 0;
        }
      
        return -1;
    }

    * condition = ENET_SOCKET_WAIT_NONE;

    if (selectCount == 0)
      return 0;

    if (FD_ISSET (socket, & writeSet))
      * condition |= ENET_SOCKET_WAIT_SEND;

    if (FD_ISSET (socket, & readSet))
      * condition |= ENET_SOCKET_WAIT_RECEIVE;

    return 0;
#endif
}

#endif

// ----- win32.c -----

#ifdef _WIN32

#include <winsock2.h>
#include <mmsystem.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")

static enet_uint32 timeBase = 0;

int
enet_initialize (void)
{
    WORD versionRequested = MAKEWORD (1, 1);
    WSADATA wsaData;
   
    if (WSAStartup (versionRequested, & wsaData))
       return -1;

    if (LOBYTE (wsaData.wVersion) != 1||
        HIBYTE (wsaData.wVersion) != 1)
    {
       WSACleanup ();
       
       return -1;
    }

    timeBeginPeriod (1);

    return 0;
}

void
enet_deinitialize (void)
{
    timeEndPeriod (1);

    WSACleanup ();
}

enet_uint32
enet_host_random_seed (void)
{
    return (enet_uint32) timeGetTime ();
}

enet_uint32
enet_time_get (void)
{
    return (enet_uint32) timeGetTime () - timeBase;
}

void
enet_time_set (enet_uint32 newTimeBase)
{
    timeBase = (enet_uint32) timeGetTime () - newTimeBase;
}

int
enet_address_set_host_ip (ENetAddress * address, const char * name)
{
    enet_uint8 vals [4] = { 0, 0, 0, 0 };
    int i;

    for (i = 0; i < 4; ++ i)
    {
        const char * next = name + 1;
        if (* name != '0')
        {
            long val = strtol (name, (char **) & next, 10);
            if (val < 0 || val > 255 || next == name || next - name > 3)
              return -1;
            vals [i] = (enet_uint8) val;
        }

        if (* next != (i < 3 ? '.' : '\0'))
          return -1;
        name = next + 1;
    }

    memcpy (& address -> host, vals, sizeof (enet_uint32));
    return 0;
}

int
enet_address_set_host (ENetAddress * address, const char * name)
{
    struct hostent * hostEntry;

    hostEntry = gethostbyname (name);
    if (hostEntry == NULL ||
        hostEntry -> h_addrtype != AF_INET)
      return enet_address_set_host_ip (address, name);

    address -> host = * (enet_uint32 *) hostEntry -> h_addr_list [0];

    return 0;
}

int
enet_address_get_host_ip (const ENetAddress * address, char * name, size_t nameLength)
{
    char * addr = inet_ntoa (* (struct in_addr *) & address -> host);
    if (addr == NULL)
        return -1;
    else
    {
        size_t addrLen = strlen(addr);
        if (addrLen >= nameLength)
          return -1;
        memcpy (name, addr, addrLen + 1);
    }
    return 0;
}

int
enet_address_get_host (const ENetAddress * address, char * name, size_t nameLength)
{
    struct in_addr in;
    struct hostent * hostEntry;
 
    in.s_addr = address -> host;
    
    hostEntry = gethostbyaddr ((char *) & in, sizeof (struct in_addr), AF_INET);
    if (hostEntry == NULL)
      return enet_address_get_host_ip (address, name, nameLength);
    else
    {
       size_t hostLen = strlen (hostEntry -> h_name);
       if (hostLen >= nameLength)
         return -1;
       memcpy (name, hostEntry -> h_name, hostLen + 1);
    }

    return 0;
}

int
enet_socket_bind (ENetSocket socket, const ENetAddress * address)
{
    struct sockaddr_in sin;

    memset (& sin, 0, sizeof (struct sockaddr_in));

    sin.sin_family = AF_INET;

    if (address != NULL)
    {
       sin.sin_port = ENET_HOST_TO_NET_16 (address -> port);
       sin.sin_addr.s_addr = address -> host;
    }
    else
    {
       sin.sin_port = 0;
       sin.sin_addr.s_addr = INADDR_ANY;
    }

    return bind (socket,
                 (struct sockaddr *) & sin,
                 sizeof (struct sockaddr_in)) == SOCKET_ERROR ? -1 : 0;
}

int
enet_socket_get_address (ENetSocket socket, ENetAddress * address)
{
    struct sockaddr_in sin;
    int sinLength = sizeof (struct sockaddr_in);

    if (getsockname (socket, (struct sockaddr *) & sin, & sinLength) == -1)
      return -1;

    address -> host = (enet_uint32) sin.sin_addr.s_addr;
    address -> port = ENET_NET_TO_HOST_16 (sin.sin_port);

    return 0;
}

int
enet_socket_listen (ENetSocket socket, int backlog)
{
    return listen (socket, backlog < 0 ? SOMAXCONN : backlog) == SOCKET_ERROR ? -1 : 0;
}

ENetSocket
enet_socket_create (ENetSocketType type)
{
    return socket (PF_INET, type == ENET_SOCKET_TYPE_DATAGRAM ? SOCK_DGRAM : SOCK_STREAM, 0);
}

int
enet_socket_set_option (ENetSocket socket, ENetSocketOption option, int value)
{
    int result = SOCKET_ERROR;
    switch (option)
    {
        case ENET_SOCKOPT_NONBLOCK:
        {
            u_long nonBlocking = (u_long) value;
            result = ioctlsocket (socket, FIONBIO, & nonBlocking);
            break;
        }

        case ENET_SOCKOPT_BROADCAST:
            result = setsockopt (socket, SOL_SOCKET, SO_BROADCAST, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_REUSEADDR:
            result = setsockopt (socket, SOL_SOCKET, SO_REUSEADDR, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_RCVBUF:
            result = setsockopt (socket, SOL_SOCKET, SO_RCVBUF, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_SNDBUF:
            result = setsockopt (socket, SOL_SOCKET, SO_SNDBUF, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_RCVTIMEO:
            result = setsockopt (socket, SOL_SOCKET, SO_RCVTIMEO, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_SNDTIMEO:
            result = setsockopt (socket, SOL_SOCKET, SO_SNDTIMEO, (char *) & value, sizeof (int));
            break;

        case ENET_SOCKOPT_NODELAY:
            result = setsockopt (socket, IPPROTO_TCP, TCP_NODELAY, (char *) & value, sizeof (int));
            break;

        default:
            break;
    }
    return result == SOCKET_ERROR ? -1 : 0;
}

int
enet_socket_get_option (ENetSocket socket, ENetSocketOption option, int * value)
{
    int result = SOCKET_ERROR, len;
    switch (option)
    {
        case ENET_SOCKOPT_ERROR:
            len = sizeof(int);
            result = getsockopt (socket, SOL_SOCKET, SO_ERROR, (char *) value, & len);
            break;

        default:
            break;
    }
    return result == SOCKET_ERROR ? -1 : 0;
}

int
enet_socket_connect (ENetSocket socket, const ENetAddress * address)
{
    struct sockaddr_in sin;
    int result;

    memset (& sin, 0, sizeof (struct sockaddr_in));

    sin.sin_family = AF_INET;
    sin.sin_port = ENET_HOST_TO_NET_16 (address -> port);
    sin.sin_addr.s_addr = address -> host;

    result = connect (socket, (struct sockaddr *) & sin, sizeof (struct sockaddr_in));
    if (result == SOCKET_ERROR && WSAGetLastError () != WSAEWOULDBLOCK)
      return -1;

    return 0;
}

ENetSocket
enet_socket_accept (ENetSocket socket, ENetAddress * address)
{
    SOCKET result;
    struct sockaddr_in sin;
    int sinLength = sizeof (struct sockaddr_in);

    result = accept (socket, 
                     address != NULL ? (struct sockaddr *) & sin : NULL, 
                     address != NULL ? & sinLength : NULL);

    if (result == INVALID_SOCKET)
      return ENET_SOCKET_NULL;

    if (address != NULL)
    {
        address -> host = (enet_uint32) sin.sin_addr.s_addr;
        address -> port = ENET_NET_TO_HOST_16 (sin.sin_port);
    }

    return result;
}

int
enet_socket_shutdown (ENetSocket socket, ENetSocketShutdown how)
{
    return shutdown (socket, (int) how) == SOCKET_ERROR ? -1 : 0;
}

void
enet_socket_destroy (ENetSocket socket)
{
    if (socket != INVALID_SOCKET)
      closesocket (socket);
}

int
enet_socket_send (ENetSocket socket,
                  const ENetAddress * address,
                  const ENetBuffer * buffers,
                  size_t bufferCount)
{
    struct sockaddr_in sin;
    DWORD sentLength = 0;

    if (address != NULL)
    {
        memset (& sin, 0, sizeof (struct sockaddr_in));

        sin.sin_family = AF_INET;
        sin.sin_port = ENET_HOST_TO_NET_16 (address -> port);
        sin.sin_addr.s_addr = address -> host;
    }

    if (WSASendTo (socket, 
                   (LPWSABUF) buffers,
                   (DWORD) bufferCount,
                   & sentLength,
                   0,
                   address != NULL ? (struct sockaddr *) & sin : NULL,
                   address != NULL ? sizeof (struct sockaddr_in) : 0,
                   NULL,
                   NULL) == SOCKET_ERROR)
    {
       if (WSAGetLastError () == WSAEWOULDBLOCK)
         return 0;

       return -1;
    }

    return (int) sentLength;
}

int
enet_socket_receive (ENetSocket socket,
                     ENetAddress * address,
                     ENetBuffer * buffers,
                     size_t bufferCount)
{
    INT sinLength = sizeof (struct sockaddr_in);
    DWORD flags = 0,
          recvLength = 0;
    struct sockaddr_in sin;

    if (WSARecvFrom (socket,
                     (LPWSABUF) buffers,
                     (DWORD) bufferCount,
                     & recvLength,
                     & flags,
                     address != NULL ? (struct sockaddr *) & sin : NULL,
                     address != NULL ? & sinLength : NULL,
                     NULL,
                     NULL) == SOCKET_ERROR)
    {
       switch (WSAGetLastError ())
       {
       case WSAEWOULDBLOCK:
       case WSAECONNRESET:
          return 0;
       }

       return -1;
    }

    if (flags & MSG_PARTIAL)
      return -1;

    if (address != NULL)
    {
        address -> host = (enet_uint32) sin.sin_addr.s_addr;
        address -> port = ENET_NET_TO_HOST_16 (sin.sin_port);
    }

    return (int) recvLength;
}

int
enet_socketset_select (ENetSocket maxSocket, ENetSocketSet * readSet, ENetSocketSet * writeSet, enet_uint32 timeout)
{
    struct timeval timeVal;

    timeVal.tv_sec = timeout / 1000;
    timeVal.tv_usec = (timeout % 1000) * 1000;

    return select (maxSocket + 1, readSet, writeSet, NULL, & timeVal);
}

int
enet_socket_wait (ENetSocket socket, enet_uint32 * condition, enet_uint32 timeout)
{
    fd_set readSet, writeSet;
    struct timeval timeVal;
    int selectCount;
    
    timeVal.tv_sec = timeout / 1000;
    timeVal.tv_usec = (timeout % 1000) * 1000;
    
    FD_ZERO (& readSet);
    FD_ZERO (& writeSet);

    if (* condition & ENET_SOCKET_WAIT_SEND)
      FD_SET (socket, & writeSet);

    if (* condition & ENET_SOCKET_WAIT_RECEIVE)
      FD_SET (socket, & readSet);

    selectCount = select (socket + 1, & readSet, & writeSet, NULL, & timeVal);

    if (selectCount < 0)
      return -1;

    * condition = ENET_SOCKET_WAIT_NONE;

    if (selectCount == 0)
      return 0;

    if (FD_ISSET (socket, & writeSet))
      * condition |= ENET_SOCKET_WAIT_SEND;
    
    if (FD_ISSET (socket, & readSet))
      * condition |= ENET_SOCKET_WAIT_RECEIVE;

    return 0;
} 

#endif

void ENET_SOCKETSET_EMPTY_PTR(ENetSocketSet* socketSet) {
    FD_ZERO(socketSet);
}
void ENET_SOCKETSET_ADD_PTR(ENetSocketSet* socketSet, ENetSocket socket) {
    FD_SET(socket, socketSet);
}
void ENET_SOCKETSET_CLR_PTR(ENetSocketSet* socketSet, ENetSocket socket) {
    FD_CLR(socket, socketSet);
}
int  ENET_SOCKETSET_CHECK_PTR(ENetSocketSet* socketSet, ENetSocket socket) {
    return FD_ISSET(socket, socketSet);
}