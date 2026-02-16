#include "save_sync.h"

#include "logging.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <minizip/unzip.h>
#include <minizip/zip.h>
#include <shlobj.h>
#include <sstream>
#include <zlib.h>

namespace save_sync {

  // JSON parser functions - extract from nested data object
  std::string extract_json_string(const std::string &json, const std::string &key) {
    // First try to find within "data" object
    std::string data_search = "\"data\":{";
    size_t data_start = json.find(data_search);
    if (data_start != std::string::npos) {
      data_start += data_search.length();
      // Find the end of data object (look for closing brace, but be careful with nested objects)
      size_t data_end = json.find("}", data_start);
      if (data_end != std::string::npos) {
        std::string data_section = json.substr(data_start, data_end - data_start);
        std::string search = "\"" + key + "\":\"";
        size_t start = data_section.find(search);
        if (start != std::string::npos) {
          start += search.length();
          size_t end = data_section.find("\"", start);
          if (end != std::string::npos) {
            return data_section.substr(start, end - start);
          }
        }
      }
    }

    // Fallback: try direct search (for backward compatibility)
    std::string search = "\"" + key + "\":\"";
    size_t start = json.find(search);
    if (start != std::string::npos) {
      start += search.length();
      size_t end = json.find("\"", start);
      if (end != std::string::npos) {
        return json.substr(start, end - start);
      }
    }

    return "";
  }

  bool extract_json_bool(const std::string &json, const std::string &key) {
    // First try to find within "data" object
    std::string data_search = "\"data\":{";
    size_t data_start = json.find(data_search);
    if (data_start != std::string::npos) {
      data_start += data_search.length();
      size_t data_end = json.find("}", data_start);
      if (data_end != std::string::npos) {
        std::string data_section = json.substr(data_start, data_end - data_start);
        std::string search = "\"" + key + "\":";
        size_t start = data_section.find(search);
        if (start != std::string::npos) {
          start += search.length();
          // Skip whitespace
          while (start < data_section.length() && (data_section[start] == ' ' || data_section[start] == '\t')) {
            start++;
          }
          return data_section.substr(start, 4) == "true";
        }
      }
    }

    // Fallback: try direct search
    std::string search = "\"" + key + "\":";
    size_t start = json.find(search);
    if (start != std::string::npos) {
      start += search.length();
      while (start < json.length() && (json[start] == ' ' || json[start] == '\t')) {
        start++;
      }
      return json.substr(start, 4) == "true";
    }

    return false;
  }

  std::string expand_windows_path(const std::string &path) {
    std::string result = path;

    char *user_profile = nullptr;
    size_t len = 0;
    _dupenv_s(&user_profile, &len, "USERPROFILE");

    if (user_profile) {
      size_t pos = result.find("%USERPROFILE%");
      if (pos != std::string::npos) {
        result.replace(pos, 13, user_profile);
      }
      free(user_profile);
    }

    return result;
  }

  // Extract folder path from save_location
  // The backend should return a Windows path (e.g., "C:\Users\...\Saves" or "%USERPROFILE%\Documents\Saves")
  // If it's a URL (like "https://example.com/save-location"), it's treated as invalid/placeholder
  std::string extract_path_from_save_location(const std::string &save_location) {
    // If it's a URL (contains ://), it's likely a placeholder - return empty to indicate invalid
    if (save_location.find("://") != std::string::npos) {
      std::cout << "[SaveSync] Warning: save_location appears to be a URL (placeholder): " << save_location << std::endl;
      return "";  // Invalid - backend should return actual Windows path
    }

    // Check if it looks like a Windows path (starts with drive letter or contains backslashes)
    // Or contains environment variables like %USERPROFILE%
    if (save_location.empty()) {
      return "";
    }

    // If it starts with a drive letter (C:, D:, etc.) or contains %...%, treat as Windows path
    if ((save_location.length() >= 2 && save_location[1] == ':' &&
         ((save_location[0] >= 'A' && save_location[0] <= 'Z') ||
          (save_location[0] >= 'a' && save_location[0] <= 'z'))) ||
        save_location.find('%') != std::string::npos ||
        save_location.find('\\') != std::string::npos) {
      return save_location;  // Valid Windows path
    }

    // If it doesn't look like a path, might be invalid
    std::cout << "[SaveSync] Warning: save_location doesn't look like a valid Windows path: " << save_location << std::endl;
    return save_location;  // Return as-is, let expand_windows_path handle it
  }

  // Compress entire folder to ZIP archive using minizip
  bool compress_folder(const std::string &folder_path, const std::string &output_zip) {
    try {
      if (!fs::exists(folder_path) || !fs::is_directory(folder_path)) {
        BOOST_LOG(debug) << "[SaveSync] Folder not found: " << folder_path << std::endl;
        return false;
      }

      zipFile zf = zipOpen(output_zip.c_str(), APPEND_STATUS_CREATE);
      if (!zf) {
        BOOST_LOG(debug) << "[SaveSync] Cannot create zip file: " << output_zip << std::endl;
        return false;
      }

      // Iterate through all files in folder
      fs::recursive_directory_iterator end_iter;
      for (fs::recursive_directory_iterator iter(folder_path); iter != end_iter; ++iter) {
        if (fs::is_regular_file(iter->status())) {
          fs::path relative = fs::relative(iter->path(), folder_path);
          std::string rel_str = relative.string();

          // Convert backslashes to forward slashes for ZIP format
          std::replace(rel_str.begin(), rel_str.end(), '\\', '/');

          zip_fileinfo zip_info = {0};
          zip_info.dosDate = 0;
          zip_info.internal_fa = 0;
          zip_info.external_fa = 0;

          int err = zipOpenNewFileInZip(zf, rel_str.c_str(), &zip_info, nullptr, 0, nullptr, 0, nullptr, Z_DEFLATED, Z_DEFAULT_COMPRESSION);
          if (err != ZIP_OK) {
            BOOST_LOG(debug) << "[SaveSync] Error opening file in zip: " << rel_str << std::endl;
            continue;
          }

          // Read and write file content
          std::ifstream input_file(iter->path().string(), std::ios::binary);
          if (input_file) {
            std::vector<uint8_t> file_content(
              (std::istreambuf_iterator<char>(input_file)),
              std::istreambuf_iterator<char>()
            );
            input_file.close();

            if (!file_content.empty()) {
              err = zipWriteInFileInZip(zf, file_content.data(), file_content.size());
              if (err != ZIP_OK) {
                BOOST_LOG(debug) << "[SaveSync] Error writing file to zip: " << rel_str << std::endl;
              } else {
                std::cout << "[SaveSync] Added to archive: " << rel_str
                          << " (" << file_content.size() << " bytes)" << std::endl;
              }
            }
          }

          zipCloseFileInZip(zf);
        }
      }

      zipClose(zf, nullptr);
      std::cout << "[SaveSync] Created ZIP archive: " << output_zip << std::endl;
      return true;

    } catch (std::exception const &e) {
      BOOST_LOG(debug) << "[SaveSync] Compression error: " << e.what() << std::endl;
      return false;
    }
  }

  // Decompress ZIP archive to folder using minizip
  bool decompress_folder(const std::string &zip_path, const std::string &output_folder) {
    try {
      if (!fs::exists(zip_path)) {
        std::cout << "[SaveSync] Zip file not found: " << zip_path << std::endl;
        return true;  // Not an error, just no save yet
      }

      unzFile uf = unzOpen(zip_path.c_str());
      if (!uf) {
        BOOST_LOG(debug) << "[SaveSync] Cannot open zip file: " << zip_path << std::endl;
        return false;
      }

      fs::create_directories(output_folder);

      // Get global info
      unz_global_info gi;
      int err = unzGetGlobalInfo(uf, &gi);
      if (err != UNZ_OK) {
        BOOST_LOG(debug) << "[SaveSync] Error reading zip global info" << std::endl;
        unzClose(uf);
        return false;
      }

      // Extract all files
      for (uLong i = 0; i < gi.number_entry; i++) {
        char filename_inzip[256];
        unz_file_info file_info;

        err = unzGetCurrentFileInfo(uf, &file_info, filename_inzip, sizeof(filename_inzip), nullptr, 0, nullptr, 0);
        if (err != UNZ_OK) {
          BOOST_LOG(debug) << "[SaveSync] Error reading file info from zip" << std::endl;
          break;
        }

        // Skip directories
        if (filename_inzip[strlen(filename_inzip) - 1] == '/') {
          if (i + 1 < gi.number_entry) {
            err = unzGoToNextFile(uf);
            if (err != UNZ_OK) {
              break;
            }
          }
          continue;
        }

        // Open file for reading
        err = unzOpenCurrentFile(uf);
        if (err != UNZ_OK) {
          BOOST_LOG(debug) << "[SaveSync] Error opening file in zip: " << filename_inzip << std::endl;
          break;
        }

        // Convert forward slashes to backslashes for Windows
        std::string rel_path = filename_inzip;
        std::replace(rel_path.begin(), rel_path.end(), '/', '\\');
        fs::path output_path = fs::path(output_folder) / rel_path;
        fs::create_directories(output_path.parent_path());

        // Read and write file
        std::ofstream output_file(output_path.string(), std::ios::binary);
        if (output_file) {
          std::vector<char> buffer(8192);
          do {
            err = unzReadCurrentFile(uf, buffer.data(), buffer.size());
            if (err < 0) {
              BOOST_LOG(debug) << "[SaveSync] Error reading file from zip: " << filename_inzip << std::endl;
              break;
            }
            if (err > 0) {
              output_file.write(buffer.data(), err);
            }
          } while (err > 0);
          output_file.close();

          std::cout << "[SaveSync] Extracted: " << rel_path
                    << " (" << file_info.uncompressed_size << " bytes)" << std::endl;
        }

        unzCloseCurrentFile(uf);

        if (i + 1 < gi.number_entry) {
          err = unzGoToNextFile(uf);
          if (err != UNZ_OK) {
            break;
          }
        }
      }

      unzClose(uf);
      std::cout << "[SaveSync] Folder extracted to: " << output_folder << std::endl;
      return true;

    } catch (std::exception const &e) {
      BOOST_LOG(debug) << "[SaveSync] Decompression error: " << e.what() << std::endl;
      return false;
    }
  }

  std::optional<GameInfo> get_game_info(
    const std::string &host,
    const std::string &port,
    int game_wb_id
  ) {
    try {
      asio_net::io_context ioc;
      tcp::resolver resolver(ioc);
      beast::tcp_stream stream(ioc);

      auto const results = resolver.resolve(host, port);
      stream.connect(results);

      http::request<http::string_body> req {
        http::verb::get,
        "/api/app/" + std::to_string(game_wb_id),
        11
      };
      req.set(http::field::host, host);
      req.set(http::field::user_agent, "Sunshine");
      req.set(http::field::accept, "application/json");

      http::write(stream, req);

      beast::flat_buffer buffer;
      http::response<http::string_body> res;
      http::read(stream, buffer, res);

      beast::error_code ec;
      stream.socket().shutdown(tcp::socket::shutdown_both, ec);

      if (res.result() == http::status::ok) {
        std::string body = res.body();

        GameInfo info;
        info.save_location = extract_json_string(body, "save_location");
        info.savable = extract_json_bool(body, "savable");

        // Extract actual path from save_location
        std::string extracted_path = extract_path_from_save_location(info.save_location);
        if (!extracted_path.empty()) {
          info.save_location = extracted_path;
        }

        std::cout << "[SaveSync] Got game info - savable: " << info.savable
                  << ", path: " << info.save_location << std::endl;

        return info;
      } else {
        BOOST_LOG(debug) << "[SaveSync] API returned: " << res.result_int() << std::endl;
      }

    } catch (std::exception const &e) {
      BOOST_LOG(debug) << "[SaveSync] Error getting game info: " << e.what() << std::endl;
    }

    return std::nullopt;
  }

  bool download_save(
    const std::string &host,
    const std::string &port,
    const std::string &user_wb_id,
    int game_wb_id,
    const std::string &save_path
  ) {
    try {
      asio_net::io_context ioc;
      tcp::resolver resolver(ioc);
      beast::tcp_stream stream(ioc);

      auto const results = resolver.resolve(host, port);
      stream.connect(results);

      std::string target = "/api/saves/" + user_wb_id + "/" + std::to_string(game_wb_id);

      http::request<http::string_body> req {http::verb::get, target, 11};
      req.set(http::field::host, host);
      req.set(http::field::user_agent, "Sunshine");

      http::write(stream, req);

      beast::flat_buffer buffer;
      http::response<http::string_body> res;
      http::read(stream, buffer, res);

      beast::error_code ec;
      stream.socket().shutdown(tcp::socket::shutdown_both, ec);

      if (res.result() == http::status::ok) {
        // Check if response is actually binary (ZIP) or JSON error
        auto content_type_header = res.find(http::field::content_type);
        if (content_type_header != res.end()) {
          std::string content_type = std::string(content_type_header->value());
          if (content_type.find("application/json") != std::string::npos) {
            // It's a JSON error response, not a ZIP file
            std::cout << "[SaveSync] Server returned JSON response (no save available): " << res.body() << std::endl;
            // Ensure folder exists even if no save to download
            fs::create_directories(save_path);
            return true;  // Not an error, just no save available
          }
        }

        // Ensure save folder exists
        fs::create_directories(save_path);

        // Save ZIP file temporarily
        std::string temp_zip = save_path + ".tmp.zip";
        fs::create_directories(fs::path(temp_zip).parent_path());

        std::ofstream file(temp_zip, std::ios::binary);
        if (!file) {
          BOOST_LOG(debug) << "[SaveSync] Cannot create temp zip file: " << temp_zip << std::endl;
          // Still return true to allow stream to start
          return true;
        }

        file.write(res.body().data(), res.body().size());
        file.close();

        std::cout << "[SaveSync] Downloaded " << res.body().size()
                  << " bytes to: " << temp_zip << std::endl;

        // Extract ZIP to the save folder
        decompress_folder(temp_zip, save_path);

        // Clean up temp ZIP file
        fs::remove(temp_zip);

        // Return true even if extraction failed - allow stream to start
        return true;

      } else if (res.result() == http::status::not_found) {
        std::cout << "[SaveSync] No save file found on server (404) - continuing normally" << std::endl;
        // Ensure folder exists even if no save to download
        fs::create_directories(save_path);
        return true;  // Not an error, just no save available
      } else {
        // Other HTTP errors - log but don't block stream
        std::cout << "[SaveSync] Download failed: " << res.result_int();
        if (!res.body().empty()) {
          std::cout << ", response: " << res.body();
        }
        std::cout << " - continuing normally" << std::endl;
        // Ensure folder exists
        fs::create_directories(save_path);
        return true;  // Return true to allow stream to start
      }

    } catch (std::exception const &e) {
      // Log error but don't block stream
      std::cout << "[SaveSync] Error downloading (non-critical): " << e.what() << " - continuing normally" << std::endl;
      // Ensure folder exists
      try {
        fs::create_directories(save_path);
      } catch (...) {
        // Ignore folder creation errors
      }
      return true;  // Return true to allow stream to start
    }
  }

  bool upload_save(
    const std::string &host,
    const std::string &port,
    const std::string &user_wb_id,
    int game_wb_id,
    const std::string &save_path
  ) {
    std::string temp_zip;
    try {
      // save_path should be the folder path directly
      if (!fs::exists(save_path) || !fs::is_directory(save_path)) {
        std::cout << "[SaveSync] No save folder to upload: " << save_path << std::endl;
        return true;
      }

      // Compress folder to ZIP first
      temp_zip = save_path + ".tmp.zip";
      if (!compress_folder(save_path, temp_zip)) {
        return false;
      }

      // Read ZIP file
      std::ifstream file(temp_zip, std::ios::binary);
      if (!file) {
        BOOST_LOG(debug) << "[SaveSync] Cannot open zip file" << std::endl;
        if (fs::exists(temp_zip)) {
          fs::remove(temp_zip);
        }
        return false;
      }

      std::string file_content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
      file.close();

      asio_net::io_context ioc;
      tcp::resolver resolver(ioc);
      beast::tcp_stream stream(ioc);

      auto const results = resolver.resolve(host, port);
      stream.connect(results);

      std::string boundary = "----SunshineBoundary";
      std::string body;

      body += "--" + boundary + "\r\n";
      body += "Content-Disposition: form-data; name=\"save_file\"; filename=\"save.zip\"\r\n";
      body += "Content-Type: application/zip\r\n\r\n";
      body += file_content;
      body += "\r\n";
      body += "--" + boundary + "--\r\n";

      std::string target = "/api/saves/" + user_wb_id + "/" + std::to_string(game_wb_id);
      http::request<http::string_body> req {http::verb::post, target, 11};
      req.set(http::field::host, host);
      req.set(http::field::user_agent, "Sunshine");
      req.set(http::field::content_type, "multipart/form-data; boundary=" + boundary);
      req.body() = body;
      req.prepare_payload();

      http::write(stream, req);

      beast::flat_buffer buffer;
      http::response<http::string_body> res;
      http::read(stream, buffer, res);

      beast::error_code ec;
      stream.socket().shutdown(tcp::socket::shutdown_both, ec);

      // Clean up temp file
      if (fs::exists(temp_zip)) {
        fs::remove(temp_zip);
      }

      if (res.result() == http::status::ok || res.result() == http::status::created) {
        std::cout << "[SaveSync] Uploaded " << file_content.size()
                  << " bytes successfully (ZIP format)" << std::endl;
        return true;
      } else {
        BOOST_LOG(debug) << "[SaveSync] Upload failed: " << res.result_int();
        if (!res.body().empty()) {
          BOOST_LOG(debug) << ", response: " << res.body();
        }
        BOOST_LOG(debug) << std::endl;
      }

    } catch (std::exception const &e) {
      BOOST_LOG(debug) << "[SaveSync] Error uploading: " << e.what() << std::endl;
      // Clean up temp file on error
      if (!temp_zip.empty() && fs::exists(temp_zip)) {
        fs::remove(temp_zip);
      }
    }

    return false;
  }

}  // namespace save_sync
