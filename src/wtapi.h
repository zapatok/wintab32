/*
 * wtapi.h - WinTab 1.4 ABI subset used by the shim and by compatibility tests.
 *
 * These are interface facts (values defined by the WinTab specification and by
 * pktdef.h); the file is deliberately independent from Wine's wintab.h so the
 * DLL does not pull LGPL headers into the build.
 */
#ifndef WTAPI_H
#define WTAPI_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef DWORD  WTPKT;
typedef DWORD  FIX32;
typedef void  *HCTX;
typedef HCTX  *LPHCTX;

/* ---- messages posted to the context window --------------------------- */
#define WT_DEFBASE      0x7FF0
#define WT_MAXOFFSET    0x000F
#define WT_PACKET       0x7FF0   /* WT_DEFBASE + 0 */
#define WT_CTXOPEN      0x7FF1
#define WT_CTXCLOSE     0x7FF2
#define WT_CTXUPDATE    0x7FF3
#define WT_CTXOVERLAP   0x7FF4
#define WT_PROXIMITY    0x7FF5   /* WT_DEFBASE + 5 */
#define WT_INFOCHANGE   0x7FF6
#define WT_CSRCHANGE    0x7FF7

/* ---- WTInfo categories ----------------------------------------------- */
#define WTI_INTERFACE   1
#define WTI_STATUS      2
#define WTI_DEFCONTEXT  3
#define WTI_DEFSYSCTX   4
#define WTI_DEVICES     100
#define WTI_CURSORS     200
#define WTI_DDCTXS      400
#define WTI_DSCTXS      500

/* WTI_INTERFACE indices */
#define IFC_WINTABID      1
#define IFC_SPECVERSION   2
#define IFC_IMPLVERSION   3
#define IFC_NDEVICES      4
#define IFC_NCURSORS      5
#define IFC_NCONTEXTS     6
#define IFC_CTXOPTIONS    7
#define IFC_CTXSAVESIZE   8
#define IFC_NEXTENSIONS   9
#define IFC_NMANAGERS    10

/* WTI_STATUS indices */
#define STA_CONTEXTS    1
#define STA_SYSCTXS     2
#define STA_PKTRATE     3
#define STA_PKTDATA     4
#define STA_MANAGERS    5
#define STA_SYSTEM      6
#define STA_BUTTONUSE   7
#define STA_SYSBTNUSE   8

/* context queries (WTI_DEFCONTEXT index != 0) */
#define CTX_NAME        1
#define CTX_OPTIONS     2
#define CTX_STATUS      3
#define CTX_LOCKS       4
#define CTX_MSGBASE     5
#define CTX_DEVICE      6
#define CTX_PKTRATE     7
#define CTX_PKTDATA     8
#define CTX_PKTMODE     9
#define CTX_MOVEMASK   10
#define CTX_BTNDNMASK  11
#define CTX_BTNUPMASK  12
#define CTX_INORGX     13
#define CTX_INORGY     14
#define CTX_INORGZ     15
#define CTX_INEXTX     16
#define CTX_INEXTY     17
#define CTX_INEXTZ     18
#define CTX_OUTORGX    19
#define CTX_OUTORGY    20
#define CTX_OUTORGZ    21
#define CTX_OUTEXTX    22
#define CTX_OUTEXTY    23
#define CTX_OUTEXTZ    24
#define CTX_SENSX      25
#define CTX_SENSY      26
#define CTX_SENSZ      27
#define CTX_SYSMODE    28
#define CTX_SYSORGX    29
#define CTX_SYSORGY    30
#define CTX_SYSEXTX    31
#define CTX_SYSEXTY    32
#define CTX_SYSSENSX   33
#define CTX_SYSSENSY   34

/* WTI_DEVICES indices */
#define DVC_NAME          1
#define DVC_HARDWARE      2
#define DVC_NCSRTYPES     3
#define DVC_FIRSTCSR      4
#define DVC_PKTRATE       5
#define DVC_PKTDATA       6
#define DVC_PKTMODE       7
#define DVC_CSRDATA       8
#define DVC_X            12
#define DVC_Y            13
#define DVC_Z            14
#define DVC_NPRESSURE    15
#define DVC_TPRESSURE    16
#define DVC_ORIENTATION  17
#define DVC_ROTATION     18
#define DVC_PNPID        19

/* WTI_CURSORS indices */
#define CSR_NAME          1
#define CSR_ACTIVE        2
#define CSR_PKTDATA       3
#define CSR_BUTTONS       4
#define CSR_BUTTONBITS    5
#define CSR_BTNNAMES      6
#define CSR_BUTTONMAP     7
#define CSR_SYSBTNMAP     8
#define CSR_NPBUTTON      9
#define CSR_NPBTNMARKS   10
#define CSR_NPRESPONSE   11
#define CSR_TPBUTTON     12
#define CSR_TPBTNMARKS   13
#define CSR_TPRESPONSE   14
#define CSR_PHYSID       15
#define CSR_MODE         16
#define CSR_MINPKTDATA   17
#define CSR_MINBUTTONS   18
#define CSR_CAPABILITIES 19
#define CSR_TYPE         20

/* DVC_HARDWARE bits */
#define HWC_INTEGRATED     0x0001
#define HWC_TOUCH          0x0002
#define HWC_HARDPROX       0x0004
#define HWC_PHYSID_CURSORS 0x0008

/* context options */
#define CXO_SYSTEM      0x0001
#define CXO_PEN         0x0002
#define CXO_MESSAGES    0x0004
#define CXO_CSRMESSAGES 0x0008
#define CXO_MGNINSIDE   0x4000
#define CXO_MARGIN      0x8000

/* context status */
#define CXS_DISABLED    0x0001
#define CXS_OBSCURED    0x0002
#define CXS_ONTOP       0x0004

/* context locks */
#define CXL_INSIZE      0x0001
#define CXL_INASPECT    0x0002
#define CXL_SENSITIVITY 0x0004
#define CXL_MARGIN      0x0008
#define CXL_SYSOUT      0x0010

/* packet status */
#define TPS_PROXIMITY   0x0001
#define TPS_QUEUE_ERR   0x0002
#define TPS_MARGIN      0x0004
#define TPS_GRAB        0x0008
#define TPS_INVERT      0x0010

/* packet data mask */
#define PK_CONTEXT          0x0001
#define PK_STATUS           0x0002
#define PK_TIME             0x0004
#define PK_CHANGED          0x0008
#define PK_SERIAL_NUMBER    0x0010
#define PK_CURSOR           0x0020
#define PK_BUTTONS          0x0040
#define PK_X                0x0080
#define PK_Y                0x0100
#define PK_Z                0x0200
#define PK_NORMAL_PRESSURE  0x0400
#define PK_TANGENT_PRESSURE 0x0800
#define PK_ORIENTATION      0x1000
#define PK_ROTATION         0x2000

/* units */
#define TU_NONE        0
#define TU_INCHES      1
#define TU_CENTIMETERS 2

#define LCNAMELEN 40

typedef struct tagAXIS {
    LONG  axMin;
    LONG  axMax;
    UINT  axUnits;
    FIX32 axResolution;
} AXIS, *PAXIS, *LPAXIS;

typedef struct tagORIENTATION {
    int orAzimuth;
    int orAltitude;
    int orTwist;
} ORIENTATION, *PORIENTATION, *LPORIENTATION;

typedef struct tagROTATION {
    LONG roPitch;
    LONG roRoll;
    LONG roYaw;
} ROTATION, *PROTATION, *LPROTATION;

typedef struct tagLOGCONTEXTA {
    char  lcName[LCNAMELEN];
    UINT  lcOptions;
    UINT  lcStatus;
    UINT  lcLocks;
    UINT  lcMsgBase;
    UINT  lcDevice;
    UINT  lcPktRate;
    WTPKT lcPktData;
    WTPKT lcPktMode;
    WTPKT lcMoveMask;
    DWORD lcBtnDnMask;
    DWORD lcBtnUpMask;
    LONG  lcInOrgX;
    LONG  lcInOrgY;
    LONG  lcInOrgZ;
    LONG  lcInExtX;
    LONG  lcInExtY;
    LONG  lcInExtZ;
    LONG  lcOutOrgX;
    LONG  lcOutOrgY;
    LONG  lcOutOrgZ;
    LONG  lcOutExtX;
    LONG  lcOutExtY;
    LONG  lcOutExtZ;
    FIX32 lcSensX;
    FIX32 lcSensY;
    FIX32 lcSensZ;
    BOOL  lcSysMode;
    int   lcSysOrgX;
    int   lcSysOrgY;
    int   lcSysExtX;
    int   lcSysExtY;
    FIX32 lcSysSensX;
    FIX32 lcSysSensY;
} LOGCONTEXTA, *PLOGCONTEXTA, *LPLOGCONTEXTA;

typedef struct tagLOGCONTEXTW {
    WCHAR lcName[LCNAMELEN];
    UINT  lcOptions;
    UINT  lcStatus;
    UINT  lcLocks;
    UINT  lcMsgBase;
    UINT  lcDevice;
    UINT  lcPktRate;
    WTPKT lcPktData;
    WTPKT lcPktMode;
    WTPKT lcMoveMask;
    DWORD lcBtnDnMask;
    DWORD lcBtnUpMask;
    LONG  lcInOrgX;
    LONG  lcInOrgY;
    LONG  lcInOrgZ;
    LONG  lcInExtX;
    LONG  lcInExtY;
    LONG  lcInExtZ;
    LONG  lcOutOrgX;
    LONG  lcOutOrgY;
    LONG  lcOutOrgZ;
    LONG  lcOutExtX;
    LONG  lcOutExtY;
    LONG  lcOutExtZ;
    LONG  lcSensX;
    LONG  lcSensY;
    LONG  lcSensZ;
    BOOL  lcSysMode;
    int   lcSysOrgX;
    int   lcSysOrgY;
    int   lcSysExtX;
    int   lcSysExtY;
    LONG  lcSysSensX;
    LONG  lcSysSensY;
} LOGCONTEXTW, *LPLOGCONTEXTW;

#ifdef __cplusplus
}
#endif

#endif /* WTAPI_H */
