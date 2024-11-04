#include <cstring>
#include <ctime>
#include <cctype>
#include <switch.h>
#include "scope_guard.hpp"
#include "internal.h"
#include <arm_neon.h>

extern "C" {
extern u8 __tls_start[];

u32 __nx_fs_num_sessions = 1;

void __libnx_init(void*, Handle, void*);
void __libnx_exit(int) {}

/* Exception handling. */
alignas(16) u8 __nx_exception_stack[0];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

void s_printf(char *out_buf, const char *fmt, ...);
}

#define R_ABORT_UNLESS(res_expr)                 \
    ({                                           \
        const auto _tmp_r_abort_rc = (res_expr); \
        if (R_FAILED(_tmp_r_abort_rc)) {         \
            diagAbortWithResult(_tmp_r_abort_rc);\
        }                                        \
    })

void __libnx_init(void*, Handle main_thread, void*) {
    // Initialize thread vars for the main thread
    ThreadVars* tv = getThreadVars();
    tv->magic      = THREADVARS_MAGIC;
    tv->thread_ptr = NULL;
    tv->reent      = _impure_ptr;
    tv->tls_tp     = (void*)((uintptr_t)__tls_start-2*sizeof(void*)); // subtract size of Thread Control Block (TCB)
    tv->handle     = main_thread;

    while ((armGetSystemTick() / armGetSystemTickFreq()) < 10)
        svcSleepThread(1'000'000'000);

    R_ABORT_UNLESS(smInitialize());
    {
        R_ABORT_UNLESS(setsysInitialize());
        {
            SetSysFirmwareVersion version;
            R_ABORT_UNLESS(setsysGetFirmwareVersion(&version));
            hosversionSet(MAKEHOSVERSION(version.major, version.minor, version.micro));
            setsysExit();
        }

        if (hosversionBefore(3, 0, 0))
            svcExitProcess();

        R_ABORT_UNLESS(hidsysInitialize());
        R_ABORT_UNLESS(capsscInitialize());
        R_ABORT_UNLESS(fsInitialize());
    }
    smExit();
}

struct bmp_t {
    u16 magic;
    u32 size;
    u32 rsvd;
    u32 data_off;
    u32 hdr_size;
    u32 width;
    u32 height;
    u16 planes;
    u16 pxl_bits;
    u32 comp;
    u32 img_size;
    u32 res_h;
    u32 res_v;
    u64 rsvd2;
} __attribute__((packed));

constexpr const u32 InComponents  = 4;
constexpr const u32 OutComponents = 3;
constexpr const u32 Width         = 1280;
constexpr const u32 Height        = 720;

constexpr const u32 InLineSize  = Width * InComponents;
constexpr const u32 OutLineSize = Width * OutComponents;

constexpr const u32 InSize   = InLineSize * Height;
constexpr const u32 OutSize  = OutLineSize * Height;
constexpr const u32 FileSize = OutSize + 0x36;

constexpr const u64 divider       = 10;
constexpr const u32 InBufferSize  = InLineSize * divider;
constexpr const u32 OutBufferSize = OutLineSize * divider;

static_assert((Height % divider) == 0);

constexpr const bmp_t bmp = {
    .magic    = 0x4D42,
    .size     = FileSize,
    .rsvd     = 0,
    .data_off = 0x36,
    .hdr_size = 40,
    .width    = Width,
    .height   = Height,
    .planes   = 1,
    .pxl_bits = 24,
    .comp     = 0,
    .img_size = OutSize,
    .res_h    = 2834,
    .res_v    = 2834,
    .rsvd2    = 0,
};

[[gnu::aligned(0x40)]]
static u8 in_buffer[InBufferSize];
[[gnu::aligned(0x40)]]
static u8 out_buffer[OutBufferSize];

char path_buffer[FS_MAX_PATH];
char b_path_buffer[FS_MAX_PATH];

#define R_TRY(res_expr)        \
    ({                         \
        auto res = (res_expr); \
        if (R_FAILED(res))     \
            return res;        \
    })

#define MAX_PATH_BUFFER 769

#if NO_JPG_DIRECTIVE

// Function to find and delete the closest .jpg to the specified reference timestamp in the Album directory
void deleteClosestToCurrentTimeJpg(FsFileSystem *albumDirectory, u64 referenceTimestamp) {
    FsDir rootDir;
    FsDirectoryEntry rootEntry;
    char closestFilePath[FS_MAX_PATH] = {0};
    u64 smallestTimeDelta = (u64)(-1);  // Initialize to max possible value

    // Open the Album directory
    if (R_FAILED(fsFsOpenDirectory(albumDirectory, "/", FsDirOpenMode_ReadDirs, &rootDir))) {
        return;
    }

    s64 entriesRead = 0;
    while (R_SUCCEEDED(fsDirRead(&rootDir, &entriesRead, 1, &rootEntry)) && entriesRead > 0) {
        if (rootEntry.type != FsDirEntryType_Dir) continue;

        char yearFolder[FS_MAX_PATH] = "/";
        std::strncat(yearFolder, rootEntry.name, FS_MAX_PATH - std::strlen(yearFolder) - 1);

        FsDir yearDir;
        if (R_FAILED(fsFsOpenDirectory(albumDirectory, yearFolder, FsDirOpenMode_ReadDirs, &yearDir))) continue;

        FsDirectoryEntry monthEntry;
        s64 monthEntriesRead = 0;
        while (R_SUCCEEDED(fsDirRead(&yearDir, &monthEntriesRead, 1, &monthEntry)) && monthEntriesRead > 0) {
            if (monthEntry.type != FsDirEntryType_Dir) continue;

            char monthFolder[FS_MAX_PATH];
            std::strncpy(monthFolder, yearFolder, FS_MAX_PATH - 1);
            std::strncat(monthFolder, "/", FS_MAX_PATH - std::strlen(monthFolder) - 1);
            std::strncat(monthFolder, monthEntry.name, FS_MAX_PATH - std::strlen(monthFolder) - 1);

            FsDir monthDir;
            if (R_FAILED(fsFsOpenDirectory(albumDirectory, monthFolder, FsDirOpenMode_ReadDirs, &monthDir))) continue;

            FsDirectoryEntry dayEntry;
            s64 dayEntriesRead = 0;
            while (R_SUCCEEDED(fsDirRead(&monthDir, &dayEntriesRead, 1, &dayEntry)) && dayEntriesRead > 0) {
                if (dayEntry.type != FsDirEntryType_Dir) continue;

                char dayFolder[FS_MAX_PATH];
                std::strncpy(dayFolder, monthFolder, FS_MAX_PATH - 1);
                std::strncat(dayFolder, "/", FS_MAX_PATH - std::strlen(dayFolder) - 1);
                std::strncat(dayFolder, dayEntry.name, FS_MAX_PATH - std::strlen(dayFolder) - 1);

                FsDir dayDir;
                if (R_FAILED(fsFsOpenDirectory(albumDirectory, dayFolder, FsDirOpenMode_ReadFiles, &dayDir))) continue;

                FsDirectoryEntry fileEntry;
                s64 fileEntriesRead = 0;
                while (R_SUCCEEDED(fsDirRead(&dayDir, &fileEntriesRead, 1, &fileEntry)) && fileEntriesRead > 0) {
                    if (fileEntry.type != FsDirEntryType_File) continue;

                    // Check for ".jpg" extension
                    size_t len = std::strlen(fileEntry.name);
                    if (len < 4 || std::strcmp(&fileEntry.name[len - 4], ".jpg") != 0) continue;

                    // Parse timestamp from filename
                    u64 fileTimestamp = 0;
                    for (int i = 0; i < 14 && std::isdigit(static_cast<unsigned char>(fileEntry.name[i])); ++i) {
                        fileTimestamp = fileTimestamp * 10 + (fileEntry.name[i] - '0');
                    }

                    // Calculate the time difference from the reference timestamp
                    u64 timeDelta = (fileTimestamp > referenceTimestamp) ? 
                                    (fileTimestamp - referenceTimestamp) : 
                                    (referenceTimestamp - fileTimestamp);

                    // Update if this .jpg file is the closest to the reference timestamp
                    if (timeDelta < smallestTimeDelta) {
                        smallestTimeDelta = timeDelta;
                        std::strncpy(closestFilePath, dayFolder, FS_MAX_PATH - 1);
                        std::strncat(closestFilePath, "/", FS_MAX_PATH - std::strlen(closestFilePath) - 1);
                        std::strncat(closestFilePath, fileEntry.name, FS_MAX_PATH - std::strlen(closestFilePath) - 1);
                    }
                }
                fsDirClose(&dayDir);
            }
            fsDirClose(&monthDir);
        }
        fsDirClose(&yearDir);
    }
    fsDirClose(&rootDir);

    // Delete the closest file if found
    if (smallestTimeDelta != (u64)(-1)) {
        fsFsDeleteFile(albumDirectory, closestFilePath);
    }
}


#endif


Result Capture() {
    /* Get filesystem handle. */
    FsFileSystem fs;
    R_TRY(fsOpenImageDirectoryFileSystem(&fs, FsImageDirectoryId_Sd));
    ScopeGuard fs_guard([&fs] { fsFsClose(&fs); });

    /* Create bitmap directory. */
    std::strcpy(path_buffer, "/Bitmaps/");
    Result rc = fsFsCreateDirectory(&fs, path_buffer);

    /* Path already exists. */
    if (R_FAILED(rc) && rc != 0x402)
        return rc;

    /* Make unique path. */
    std::strcpy(path_buffer, "/Bitmaps/tmp.bmp");

    /* Create file. */
    R_TRY(fsFsCreateFile(&fs, path_buffer, FileSize, 0));
    ScopeGuard rm_guard([&fs] { fsFsDeleteFile(&fs, path_buffer); });

    /* Open file. */
    FsFile file;
    R_TRY(fsFsOpenFile(&fs, path_buffer, FsOpenMode_Write, &file));
    ScopeGuard file_guard([&file] { fsFileClose(&file); });

    /* Write bitmap header. */
    off_t offset = 0;
    fsFileWrite(&file, 0, &bmp, 54, FsWriteOption_None);
    offset += 54;

    u64 written = 0;
    for (int y = (Height / divider) - 1; y >= 0; y--) {
        /* Read raw image data */
        R_TRY(capsscReadRawScreenShotReadStream(&written, in_buffer, sizeof(in_buffer), y * sizeof(in_buffer)));

        /* Resample buffer bottom up. */
        for (int div_y = (divider - 1); div_y >= 0; div_y--) {
            u8 *out = out_buffer + (div_y * OutLineSize);
            u8 *in  = in_buffer + ((divider - div_y - 1) * InLineSize);

            /* BGRX to RGB bitmap */
            for (u32 x = 0; x < Width; x += 16, in += 16 * InComponents, out += 16 * OutComponents) {
                uint8x16x4_t bgra = vld4q_u8(in);
                uint8x16x3_t rgb = { bgra.val[2], bgra.val[1], bgra.val[0] };
                vst3q_u8(out, rgb);
            }
        }

        /* Write to file. */
        R_TRY(fsFileWrite(&file, offset, out_buffer, sizeof(out_buffer), FsWriteOption_None));
        offset += sizeof(out_buffer);
    }

    rm_guard.Cancel();
    file_guard.Invoke();

    /* Get timestamp for the temporary file */
    FsTimeStampRaw timestamp;
    u64 referenceTimestamp = 0;
    if (R_SUCCEEDED(fsFsGetFileTimeStampRaw(&fs, path_buffer, &timestamp))) {
        time_t ts = timestamp.created;
        struct tm _t;
        struct tm *t = gmtime_r(&ts, &_t);

        // Generate reference timestamp in yyyymmddhhmmss format
        referenceTimestamp = (t->tm_year + 1900) * 10000000000ULL +
                             (t->tm_mon + 1) * 100000000 +
                             t->tm_mday * 1000000 +
                             t->tm_hour * 10000 +
                             t->tm_min * 100 +
                             t->tm_sec;

        // Rename the temp file to include this timestamp
        s_printf(b_path_buffer, "/Bitmaps/%d-%02d-%02d_%02d-%02d-%02d.bmp",
                 t->tm_year + 1900,
                 t->tm_mon + 1,
                 t->tm_mday,
                 t->tm_hour,
                 t->tm_min,
                 t->tm_sec);
        fsFsRenameFile(&fs, path_buffer, b_path_buffer);
    }

    #if NO_JPG_DIRECTIVE
    deleteClosestToCurrentTimeJpg(&fs, referenceTimestamp);
    #endif
    
    return 0;
}

int main() {
    bool held = false;

    u64 start_tick = 0;

    /* Obtain capture button event. */
    Event event;
    R_ABORT_UNLESS(hidsysAcquireCaptureButtonEventHandle(&event, false));
    eventClear(&event);

    /* Loop forever. */
    //while (true) {
    //    if (R_SUCCEEDED(eventWait(&event, 17'000'000))) {
    //        eventClear(&event);
    //        if (!held) {
    //                            // If button was not already held, start holding
    //            held = true;
    //            start_tick = armGetSystemTick();
    //            /* Capture screen in VI. */
    //            if (R_SUCCEEDED(capsscOpenRawScreenShotReadStream(nullptr, nullptr, nullptr, ViLayerStack_Default, 100'000'000))) {
    //                held       = true;
    //                start_tick = armGetSystemTick();
    //            }
    //        } else if (start_tick != 0) {
    //            /* Capture bitmap in file. */
    //            Capture();
    //            /* Discard capture. */
    //            capsscCloseRawScreenShotReadStream();
    //            start_tick = 0;
    //            held       = false;
    //        } else {
    //            held = false;
    //        }
    //    } else {
    //        if (start_tick != 0) {
    //            /* If held for more than half a second we discard the capture. */
    //            if (armTicksToNs(armGetSystemTick() - start_tick) > 500'000'000) {
    //                capsscCloseRawScreenShotReadStream();
    //                start_tick = 0;
    //                held       = false;
    //            }
    //        }
    //    }
    //}

    //bool initialOpen = false;

    // Loop forever, waiting for capture button event.
    while (true)
    {
        // Check for button press event
        if (R_SUCCEEDED(eventWait(&captureButtonEvent, UINT64_MAX))) // await indefinetly
        {
            eventClear(&event);
            //if (initialOpen) {
            //    initialOpen = false;
            //    capsscCloseRawScreenShotReadStream();
            //    start_tick += armNsToTicks(50000000); // Convert 10 ms to ticks and add it to start_tick
            //}

            if (!held)
            {
                // If button was not already held, start holding
                held = true;
                //capsscOpenRawScreenShotReadStream(nullptr, nullptr, nullptr, ViLayerStack_Default, 100'000'000);
                //initialOpen = true;
                start_tick = armGetSystemTick();
            }
            else
            {
                // If button was already held and now released
                u64 elapsed_ns = armTicksToNs(armGetSystemTick() - start_tick);
    
                if (elapsed_ns >= 50000000 && elapsed_ns < 500000000) // Between 50 ms and 500 ms
                {
                    /* Capture bitmap in file. */
                    capsscOpenRawScreenShotReadStream(nullptr, nullptr, nullptr, ViLayerStack_Default, 100'000'000);
                    Capture();
                    capsscCloseRawScreenShotReadStream();
                }
                

                // Reset the state
                held = false;
                start_tick = 0;
                

            }
        }
        else if (held)
        {

            // If the button was held for more than 500 ms, reset
            u64 elapsed_ns = armTicksToNs(armGetSystemTick() - start_tick);
            
            if (elapsed_ns >= 500000000) // More than 500 ms
            {
                //if (initialOpen) {
                //    initialOpen = false;
                //    capsscCloseRawScreenShotReadStream();
                //}
                //capsscCloseRawScreenShotReadStream();
                // Long press detected, ignore as a quick press
                held = false;
                start_tick = 0;
            }
        }
    }


    /* Unreachable lol */
}
