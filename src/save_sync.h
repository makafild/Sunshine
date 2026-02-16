#ifndef SAVE_SYNC_H
#define SAVE_SYNC_H

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/filesystem.hpp>
#include <optional>
#include <string>
#include <vector>

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio_net = boost::asio;
namespace fs = boost::filesystem;
using tcp = asio_net::ip::tcp;

namespace save_sync {

  struct GameInfo {
    std::string save_location;
    bool savable;
  };

  std::string extract_json_string(const std::string &json, const std::string &key);
  bool extract_json_bool(const std::string &json, const std::string &key);
  std::string expand_windows_path(const std::string &path);

  std::optional<GameInfo> get_game_info(
    const std::string &host,
    const std::string &port,
    int game_wb_id
  );

  bool download_save(
    const std::string &host,
    const std::string &port,
    const std::string &user_wb_id,
    int game_wb_id,
    const std::string &save_path
  );

  bool upload_save(
    const std::string &host,
    const std::string &port,
    const std::string &user_wb_id,
    int game_wb_id,
    const std::string &save_path
  );

  // Folder compression/decompression using minizip (ZIP format)
  bool compress_folder(const std::string &folder_path, const std::string &output_zip);
  bool decompress_folder(const std::string &zip_path, const std::string &output_folder);

}  // namespace save_sync

#endif  // SAVE_SYNC_H
