/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "mpq_libmpq04.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <deque>

ArchiveSet gOpenArchives;

namespace
{
    std::string LowerAscii(std::string s)
    {
        for (char& c : s)
            if (c >= 'A' && c <= 'Z')
                c = char(c - 'A' + 'a');
        return s;
    }

    // Resolves "DBFilesClient\\Map.dbc" below a loose folder, ignoring case on every path component
    // (needed on case sensitive file systems). Returns an empty path when not found.
    std::filesystem::path ResolveLoosePath(std::string const& root, std::string const& name)
    {
        std::filesystem::path current(root);
        std::string component;
        std::vector<std::string> parts;
        for (char c : name)
        {
            if (c == '\\' || c == '/')
            {
                if (!component.empty())
                    parts.push_back(component);
                component.clear();
            }
            else
                component += c;
        }
        if (!component.empty())
            parts.push_back(component);

        std::error_code ec;
        for (std::string const& part : parts)
        {
            std::filesystem::path direct = current / part;
            if (std::filesystem::exists(direct, ec))
            {
                current = direct;
                continue;
            }

            bool found = false;
            std::string lowered = LowerAscii(part);
            for (std::filesystem::directory_iterator it(current, ec), end; !ec && it != end; it.increment(ec))
            {
                if (LowerAscii(it->path().filename().string()) == lowered)
                {
                    current = it->path();
                    found = true;
                    break;
                }
            }
            if (!found)
                return {};
        }

        return std::filesystem::is_regular_file(current, ec) ? current : std::filesystem::path();
    }
}

void CollectPatchArchives(std::string const& dir, std::string const& prefix, std::vector<std::string>& out)
{
    std::string const lowPrefix = LowerAscii(prefix);
    std::vector<std::pair<uint32, std::string>> found;   // sort key, path

    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir.empty() ? "." : dir, ec), end; !ec && it != end; it.increment(ec))
    {
        std::string name = LowerAscii(it->path().filename().string());
        if (name.size() < lowPrefix.size() + 4 || name.compare(0, lowPrefix.size(), lowPrefix) != 0 || name.compare(name.size() - 4, 4, ".mpq") != 0)
            continue;

        std::string middle = name.substr(lowPrefix.size(), name.size() - lowPrefix.size() - 4);
        uint32 key;
        if (middle.empty())
            key = 0;
        else if (middle.size() >= 2 && middle[0] == '-' && std::all_of(middle.begin() + 1, middle.end(), [](char c) { return c >= '0' && c <= '9'; }))
            key = 1 + uint32(std::min<unsigned long>(std::stoul(middle.substr(1)), 100000ul));
        else if (middle.size() == 2 && middle[0] == '-' && middle[1] >= 'a' && middle[1] <= 'z')
            key = 200000 + uint32(middle[1] - 'a');
        else
            continue;

        found.emplace_back(key, it->path().string());
    }

    std::sort(found.begin(), found.end());
    for (auto const& entry : found)
        out.push_back(entry.second);
}

MPQArchive::MPQArchive(const char* filename)
{
    {
        std::error_code ec;
        if (std::filesystem::is_directory(filename, ec))
        {
            // loose folder behaving like an archive: its content overrides older archives
            looseDir = filename;
            printf("Opening loose folder %s\n", filename);
            gOpenArchives.push_front(this);
            return;
        }
    }

    int result = libmpq__archive_open(&mpq_a, filename, -1);
    printf("Opening %s\n", filename);
    if (result)
    {
        switch (result)
        {
            case LIBMPQ_ERROR_OPEN :
                printf("Error opening archive '%s': Does file really exist?\n", filename);
                break;
            case LIBMPQ_ERROR_FORMAT :            /* bad file format */
                printf("Error opening archive '%s': Bad file format\n", filename);
                break;
            case LIBMPQ_ERROR_SEEK :         /* seeking in file failed */
                printf("Error opening archive '%s': Seeking in file failed\n", filename);
                break;
            case LIBMPQ_ERROR_READ :              /* Read error in archive */
                printf("Error opening archive '%s': Read error in archive\n", filename);
                break;
            case LIBMPQ_ERROR_MALLOC :               /* maybe not enough memory? :) */
                printf("Error opening archive '%s': Maybe not enough memory\n", filename);
                break;
            default:
                printf("Error opening archive '%s': Unknown error\n", filename);
                break;
        }
        return;
    }
    gOpenArchives.push_front(this);
}

void MPQArchive::close()
{
    if (IsLoose())
        return;

    //gOpenArchives.erase(erase(&mpq_a);
    libmpq__archive_close(mpq_a);
}

MPQFile::MPQFile(const char* filename):
    eof(false),
    buffer(nullptr),
    pointer(0),
    size(0)
{
    for (auto & gOpenArchive : gOpenArchives)
    {
        if (gOpenArchive->IsLoose())
        {
            std::filesystem::path path = ResolveLoosePath(gOpenArchive->looseDir, filename);
            if (path.empty())
                continue;

            std::ifstream stream(path, std::ios::binary);
            std::error_code sizeEc;
            std::uintmax_t fileSize = std::filesystem::file_size(path, sizeEc);
            if (!stream || sizeEc)
                continue;

            size = libmpq__off_t(fileSize);
            if (size <= 1)
            {
                eof = true;
                buffer = nullptr;
                return;
            }

            buffer = new char[size];
            stream.read(buffer, size);
            return;
        }

        mpq_archive* mpq_a = gOpenArchive->mpq_a;

        uint32_t filenum;
        if (libmpq__file_number(mpq_a, filename, &filenum)) continue;
        libmpq__off_t transferred;
        libmpq__file_unpacked_size(mpq_a, filenum, &size);

        // HACK: in patch.mpq some files don't want to open and give 1 for filesize
        if (size <= 1)
        {
            //            printf("warning: file %s has size %d; cannot read.\n", filename, size);
            eof = true;
            buffer = nullptr;
            return;
        }
        buffer = new char[size];

        //libmpq_file_getdata
        libmpq__file_read(mpq_a, filenum, (unsigned char*)buffer, size, &transferred);
        /*libmpq_file_getdata(&mpq_a, hash, fileno, (unsigned char*)buffer);*/
        return;
    }
    eof = true;
    buffer = nullptr;
}

std::size_t MPQFile::read(void* dest, std::size_t bytes)
{
    if (eof) return 0;

    std::size_t rpos = pointer + bytes;
    if (rpos > std::size_t(size))
    {
        bytes = size - pointer;
        eof = true;
    }

    memcpy(dest, &(buffer[pointer]), bytes);

    pointer = rpos;

    return bytes;
}

void MPQFile::seek(int offset)
{
    pointer = offset;
    eof = (pointer >= size);
}

void MPQFile::seekRelative(int offset)
{
    pointer += offset;
    eof = (pointer >= size);
}

void MPQFile::close()
{
    delete[] buffer;
    buffer = nullptr;
    eof = true;
}
