#include <algorithm>
#include <string>
#include <vector>
#include <3ds.h>
#include <stdio.h>

void Pause() {
    printf("Press A to continue...\n");
    u32 kDown;
    while (aptMainLoop()) {
        hidScanInput();
        kDown = hidKeysDown();

        if (kDown & KEY_A) {
            return;
        }

        gfxFlushBuffers();
        gspWaitForVBlank();
        gfxSwapBuffers();
    }
    aptExit();
    exit(0);
}

void Exit() {
    printf("Press A to exit...\n");
    u32 kDown;
    while (aptMainLoop()) {
        hidScanInput();
        kDown = hidKeysDown();

        if (kDown & KEY_A) {
            break;
        }

        gfxFlushBuffers();
        gspWaitForVBlank();
        gfxSwapBuffers();
    }
    aptExit();
    exit(0);
}

bool PrintOnError(const char* func_name, Result result) {
    bool success = R_SUCCEEDED(result);
    if (!success) {
        printf("%s: %08lX\n", func_name, result);
    }
    return success;
}

void ExitOnError(const char* func_name, Result result) {
    if (!PrintOnError(func_name, result)) {
        Exit();
    }
}

FS_Path MakePath(const std::u16string& path) {
    static std::u16string t;
    t = path;
    return FS_Path{PATH_UTF16, t.size() * 2 + 2, t.data()};
}

template <typename T>
FS_Path MakePath(const std::vector<T>& binary) {
    static std::vector<T> t;
    t = binary;
    if (t.size()) {
        return FS_Path{PATH_BINARY, t.size() * sizeof(T), t.data()};
    } else {
        return FS_Path{PATH_EMPTY, 0, nullptr};
    }
}

std::u16string Hex32ToString(u32 hex) {
    std::u16string result;
    for (int i = 0; i < 8; ++i) {
        u32 digit = hex & 0xF;
        hex >>= 4;
        if (digit < 10) {
            result = char16_t('0' + digit) + result;
        } else {
            result = char16_t('a' + digit - 10) + result;
        }
    }
    return result;
}

FS_Path MakeSDSaveBinaryPath(u64 title_id) {
    return MakePath(std::vector<u32>{1, (u32)(title_id & 0xFFFFFFFF), (u32)(title_id >> 32)});
}

std::vector<FS_DirectoryEntry> GetEntries(FS_Archive archive, const std::u16string& src_path) {
    Handle dir_handle;
    if (!PrintOnError("OpenDir", FSUSER_OpenDirectory(&dir_handle, archive, MakePath(src_path)))) {
        return {};
    }

    u32 size;
    std::vector<FS_DirectoryEntry> entries;
    for (;;) {
        FS_DirectoryEntry tmp;
        if (!PrintOnError("ReadDir", FSDIR_Read(dir_handle, &size, 1, &tmp))) {
            return {};
        }
        if (size == 0)
            break;
        entries.push_back(tmp);
    }

    PrintOnError("CloseDir", FSDIR_Close(dir_handle));

    return entries;
}

void CopyDir(FS_Archive src_archive, const std::u16string& src_path, FS_Archive dst_archive,
             const std::u16string& dst_path) {
    FSUSER_CreateDirectory(dst_archive, MakePath(dst_path), 0);
    std::u16string src_path_fix = src_path.size() == 0 ? u"/" : src_path;
    auto entries = GetEntries(src_archive, src_path_fix);
    for (auto entry : entries) {
        std::u16string sub_name((const char16_t*)entry.name);
        if (entry.attributes & FS_ATTRIBUTE_DIRECTORY) {
            CopyDir(src_archive, src_path + u"/" + sub_name, dst_archive,
                    dst_path + u"/" + sub_name);
        } else {
            Handle file;
            std::vector<u8> buffer;
            u32 bytes;
            bool failed = false;

            if (PrintOnError("OpenFile (source)",
                             FSUSER_OpenFile(&file, src_archive,
                                             MakePath(src_path + u"/" + sub_name), FS_OPEN_READ,
                                             0))) {
                u64 size = 0;
                if (!PrintOnError("GetSize (source)", FSFILE_GetSize(file, &size)))
                    failed = true;

                if (size) {
                    buffer.resize(size);
                    if (!PrintOnError("Read (source)",
                                      FSFILE_Read(file, &bytes, 0, buffer.data(), size)))
                        failed = true;
                    else if (bytes != size) {
                        printf("Read (source) size mismatch\n");
                        failed = true;
                    }
                }
                if (!PrintOnError("Close (source)", FSFILE_Close(file)))
                    failed = true;
            } else
                failed = true;

            if (PrintOnError("OpenFile (dest)",
                             FSUSER_OpenFile(&file, dst_archive,
                                             MakePath(dst_path + u"/" + sub_name),
                                             FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) {
                if (buffer.size()) {
                    if (!PrintOnError("Write (dest)", FSFILE_Write(file, &bytes, 0, buffer.data(),
                                                                   buffer.size(), 0)))
                        failed = true;
                    else if (bytes != buffer.size()) {
                        printf("Write (dest) size mismatch\n");
                        failed = true;
                    }
                }
                if (!PrintOnError("Close (dest)", FSFILE_Close(file)))
                    failed = true;
            } else
                failed = true;

            if (failed) {
                std::u16string full_src_path(src_path + u"/" + sub_name);
                std::string converted(full_src_path.size(), ' ');
                std::copy(full_src_path.begin(), full_src_path.end(), converted.begin());
                printf(" %s\n", converted.c_str());
            }
        }
    }
}

struct ArchiveFormatInfo {
    u32 total_size;
    u32 number_directories;
    u32 number_files;
    bool duplicate_data;
    u8 padding[3];
};

static_assert(sizeof(ArchiveFormatInfo) == 16, "ArchiveFormatInfo has wrong size");

void DumpSDSave(FS_Archive sd, const std::u16string& sdsave_root) {
    printf("Dumping SD save...\n");

    u32 sd_title_count;
    ExitOnError("GetTitleCount(sd)", AM_GetTitleCount(MEDIATYPE_SD, &sd_title_count));
    printf("SD title count: %lu\n", sd_title_count);

    std::vector<u64> sd_titles(sd_title_count);
    u32 sd_title_read;
    ExitOnError("GetTitleList(sd)",
                AM_GetTitleList(&sd_title_read, MEDIATYPE_SD, sd_title_count, sd_titles.data()));

    if (sd_title_count != sd_title_read) {
        printf("GetTitleList(sd) count mismatch\n");
        Exit();
    }

    for (u64 title : sd_titles) {
        if ((title >> 32) == 0x00040000) {

            FS_Archive save_archive;
            if (R_FAILED(FSUSER_OpenArchive(&save_archive, ARCHIVE_USER_SAVEDATA,
                                            MakeSDSaveBinaryPath(title)))) {
                continue;
            }

            printf("Title: %016llX\n", title);
            std::u16string save_root = sdsave_root;
            save_root += u"/" + Hex32ToString(title & 0xFFFFFFFF);
            FSUSER_CreateDirectory(sd, MakePath(save_root), 0);
            save_root += u"/data";
            FSUSER_CreateDirectory(sd, MakePath(save_root), 0);
            std::u16string save_metadata = save_root + u"/00000001.metadata";
            save_root += u"/00000001";
            FSUSER_CreateDirectory(sd, MakePath(save_root), 0);

            // Save data
            CopyDir(save_archive, u"", sd, save_root);
            PrintOnError("CloseArchive(source)", FSUSER_CloseArchive(save_archive));

            // Write metadata
            Handle file_metadata;
            if (PrintOnError("OpenFile(metadata)",
                             FSUSER_OpenFile(&file_metadata, sd, MakePath(save_metadata),
                                             FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) {
                ArchiveFormatInfo format_info;

                // NOTE: FSUSER_GetFormatInfo only works with some CategoryFileSystemTool flag on.
                //       thus currently this application must be built as CIA. It doesn't even work
                //       with Luma3DS's "Patch Archive check" (while FSUSER_OpenArchive does work
                //       with it). This is probably a bug/oversight in Luma3DS
                PrintOnError(
                    "GetFormatInfo",
                    FSUSER_GetFormatInfo(&format_info.total_size, &format_info.number_directories,
                                         &format_info.number_files, &format_info.duplicate_data,
                                         ARCHIVE_USER_SAVEDATA, MakeSDSaveBinaryPath(title)));

                PrintOnError("Write(metadata)", FSFILE_Write(file_metadata, nullptr, 0,
                                                             &format_info, sizeof(format_info), 0));

                PrintOnError("Close(metadata)", FSFILE_Close(file_metadata));
            }
        }
    }

    printf("Done\n");
}

void DumpSDExt(FS_Archive sd, const std::u16string& sdext_root) {
    printf("Dumping SD ext...\n");

    std::vector<u64> ext_ids(4);

    while (1) {
        u32 count_read;
        FSUSER_EnumerateExtSaveData(&count_read, ext_ids.size() * 8, MEDIATYPE_SD, 8, false,
                                    (u8*)ext_ids.data());
        if (count_read > ext_ids.size()) {
            printf("what??");
            return;
        } else if (count_read < ext_ids.size()) {
            ext_ids.resize(count_read);
            break;
        }

        ext_ids.resize(ext_ids.size() * 2);
    }

    printf("SD ext count: %zu\n", ext_ids.size());

    for (u64 ext_id : ext_ids) {
        printf("Ext: %016llX\n", ext_id);

        if ((ext_id >> 32) != 0) {
            printf("Unexpected non zero ID high!\n");
            continue;
        }

        std::u16string ext_root = sdext_root + u"/" + Hex32ToString(ext_id & 0xFFFFFFFF);
        FSUSER_CreateDirectory(sd, MakePath(ext_root), 0);
        std::u16string extuser_root = ext_root + u"/user";
        FSUSER_CreateDirectory(sd, MakePath(extuser_root), 0);
        std::u16string extboss_root = ext_root + u"/boss";
        FSUSER_CreateDirectory(sd, MakePath(extboss_root), 0);

        // Save data
        FS_Archive ext_archive;
        if (PrintOnError("OpenArchive", FSUSER_OpenArchive(&ext_archive, ARCHIVE_EXTDATA,
                                                           MakeSDSaveBinaryPath(ext_id)))) {

            CopyDir(ext_archive, u"", sd, extuser_root);
            PrintOnError("CloseArchive", FSUSER_CloseArchive(ext_archive));
        }

        // Write metadata
        std::u16string ext_metadata = ext_root + u"/metadata";
        Handle file_metadata;
        if (PrintOnError("OpenFile(metadata)",
                         FSUSER_OpenFile(&file_metadata, sd, MakePath(ext_metadata),
                                         FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) {
            ArchiveFormatInfo format_info;
            PrintOnError(
                "GetFormatInfo",
                FSUSER_GetFormatInfo(&format_info.total_size, &format_info.number_directories,
                                     &format_info.number_files, &format_info.duplicate_data,
                                     ARCHIVE_EXTDATA, MakeSDSaveBinaryPath(ext_id)));

            PrintOnError("Write(metadata)", FSFILE_Write(file_metadata, nullptr, 0, &format_info,
                                                         sizeof(format_info), 0));

            PrintOnError("Close(metadata)", FSFILE_Close(file_metadata));
        }

        // TODO: icon
    }

    printf("Done\n");
}

int main() {
    aptInit();
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    hidInit();

    printf("Initializing...\n");

    ExitOnError("amInit", amInit());

    FS_Path sd_path{PATH_EMPTY, 0, nullptr};
    FS_Archive sd;
    ExitOnError("OpenArchive(sd)", FSUSER_OpenArchive(&sd, ARCHIVE_SDMC, sd_path));

    std::u16string root = u"/save-to-citra", sd_root = root;
    FSUSER_DeleteDirectoryRecursively(sd, MakePath(root));
    FSUSER_CreateDirectory(sd, MakePath(root), 0);
    sd_root += u"/sdmc";
    FSUSER_CreateDirectory(sd, MakePath(sd_root), 0);
    sd_root += u"/Nintendo 3DS";
    FSUSER_CreateDirectory(sd, MakePath(sd_root), 0);
    sd_root += u"/00000000000000000000000000000000";
    FSUSER_CreateDirectory(sd, MakePath(sd_root), 0);
    sd_root += u"/00000000000000000000000000000000";
    FSUSER_CreateDirectory(sd, MakePath(sd_root), 0);

    std::u16string sdsave_root = sd_root;
    sdsave_root += u"/title";
    FSUSER_CreateDirectory(sd, MakePath(sdsave_root), 0);
    sdsave_root += u"/00040000";
    FSUSER_CreateDirectory(sd, MakePath(sdsave_root), 0);

    std::u16string sdext_root = sd_root;
    sdext_root += u"/extdata";
    FSUSER_CreateDirectory(sd, MakePath(sdext_root), 0);
    sdext_root += u"/00000000";
    FSUSER_CreateDirectory(sd, MakePath(sdext_root), 0);

    Pause();

    DumpSDSave(sd, sdsave_root);
    DumpSDExt(sd, sdext_root);

    FSUSER_CloseArchive(sd);

    printf("All done!\n");

    Pause();

    hidExit();

    gfxExit();
    aptExit();
    return 0;
}
