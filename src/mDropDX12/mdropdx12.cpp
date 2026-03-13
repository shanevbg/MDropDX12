#include "mdropdx12.h"
#include "utility.h"

MDropDX12::MDropDX12() {}

void MDropDX12::Init(wchar_t* exePath) {
  winrt::init_apartment(); // Initialize the WinRT runtime
  start_time = std::chrono::steady_clock::now();

  // Get the executable's directory
  std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();

  // Construct the "resources/sprites/" directory path relative to the executable
  std::filesystem::path spritesDir = exeDir / "resources/sprites";
  std::filesystem::create_directories(spritesDir);

  // Construct the file path
  coverSpriteFilePath = spritesDir / "cover.png";
}

void MDropDX12::PollMediaInfo() {

  if (!doPoll && !doPollExplicit) return;

  try {
    // Get the current time
    auto current_time = std::chrono::steady_clock::now();

    // Calculate the elapsed time in seconds
    auto elapsed_seconds = std::chrono::duration_cast<std::chrono::seconds>(current_time - start_time).count();

    // Check if 2 seconds have passed or manual poll requested
    if (elapsed_seconds >= 2 || doPollExplicit) {

      auto smtcManager = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
      auto currentSession = smtcManager.GetCurrentSession();
      updated = false;
      if (currentSession) {
        auto properties = currentSession.TryGetMediaPropertiesAsync().get();
        if (properties) {
          if (doPollExplicit || properties.Artist().c_str() != currentArtist || properties.Title().c_str() != currentTitle || properties.AlbumTitle().c_str() != currentAlbum) {
            isSongChange = currentAlbum.length() || currentArtist.length() || currentTitle.length();
            currentArtist = properties.Artist().c_str();
            currentTitle = properties.Title().c_str();
            currentAlbum = properties.AlbumTitle().c_str();

            if ((doPollExplicit || doSaveCover) && properties.Thumbnail()) {
              SaveThumbnailToFile(properties);
            }

            updated = true;
          }
        }
      }
      else {
        if (currentArtist.length() || currentTitle.length() || currentAlbum.length()) {
          currentArtist = L"";
          currentTitle = L"";
          currentAlbum = L"";
          updated = true;
        }
      }

      // Reset the start time to the current time
      start_time = current_time;
    }
  } catch (const std::exception& e) {
    LogException(L"PollMediaInfo", e, false);
  }
}

bool MDropDX12::SaveThumbnailToFile(const winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionMediaProperties& properties) {
  try {
    // Retrieve the thumbnail
    auto thumbnailRef = properties.Thumbnail();
    if (!thumbnailRef) {
      std::wcerr << L"No thumbnail available for the current media." << std::endl;
      return false;
    }

    // Open the thumbnail stream
    auto thumbnailStream = thumbnailRef.OpenReadAsync().get();
    auto decoder = winrt::Windows::Graphics::Imaging::BitmapDecoder::CreateAsync(thumbnailStream).get();

    // Encode the image as a PNG and save it to the file
    auto fileStream = winrt::Windows::Storage::Streams::InMemoryRandomAccessStream();
    auto encoder = winrt::Windows::Graphics::Imaging::BitmapEncoder::CreateAsync(
      winrt::Windows::Graphics::Imaging::BitmapEncoder::PngEncoderId(), fileStream).get();

    encoder.SetSoftwareBitmap(decoder.GetSoftwareBitmapAsync().get());
    encoder.FlushAsync().get();

    // Write the encoded image to the file
    std::ofstream outputFile(coverSpriteFilePath, std::ios::binary);
    if (!outputFile.is_open()) {
      std::wcerr << L"Failed to open file for writing: " << coverSpriteFilePath << std::endl;
      return false;
    }

    // Use DataReader to read the stream content
    auto size = fileStream.Size();
    fileStream.Seek(0);
    auto buffer = winrt::Windows::Storage::Streams::Buffer(static_cast<uint32_t>(size));
    fileStream.ReadAsync(buffer, static_cast<uint32_t>(size), winrt::Windows::Storage::Streams::InputStreamOptions::None).get();

    outputFile.write(reinterpret_cast<const char*>(buffer.data()), buffer.Length());
    outputFile.close();

    std::wcout << L"Thumbnail saved to: " << coverSpriteFilePath.wstring() << std::endl;
    coverUpdated = true;
    return true;
  } catch (const std::exception& e) {
    LogException(L"SaveThumbnailToFile", e, false);
  }
  return false;
}

void MDropDX12::LogDebug(std::wstring info) {
  DebugLogW(info.c_str(), LOG_VERBOSE);
}

void MDropDX12::LogDebug(const wchar_t* info) {
  DebugLogW(info, LOG_VERBOSE);
}

void MDropDX12::LogInfo(std::wstring info) {
  DebugLogW(info.c_str(), LOG_INFO);
}

void MDropDX12::LogInfo(const wchar_t* info) {
  DebugLogW(info, LOG_INFO);
}

void MDropDX12::LogException(const wchar_t* context, const std::exception& e, bool showMessage) {

  if (logLevel < 1) return;

  DebugLogWFmt(LOG_ERROR, L"caught exception: %s: %S", context, e.what());

  // Write detailed stack trace to a separate error log file
  wchar_t diagName[64];
  std::time_t now = std::time(nullptr);
  std::tm localTime;
  localtime_s(&localTime, &now);
  char timestamp[20];
  std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d_%H-%M-%S", &localTime);
  swprintf_s(diagName, L"diag_%S_error", timestamp);

  FILE* f = DebugLogDiagOpen(diagName, L"w");
  if (f) {
    fprintf(f, "Exception occurred: %S\n%s\n", context, e.what());

    // Capture and log the stack trace
    fprintf(f, "\nStack trace:\n");
    HANDLE process = GetCurrentProcess();
    SymInitialize(process, NULL, TRUE);

    void* stack[64];
    USHORT frames = CaptureStackBackTrace(0, 64, stack, NULL);

    SYMBOL_INFO* symbol = (SYMBOL_INFO*)malloc(sizeof(SYMBOL_INFO) + 256 * sizeof(char));
    if (symbol) {
      symbol->MaxNameLen = 255;
      symbol->SizeOfStruct = sizeof(SYMBOL_INFO);

      for (USHORT i = 0; i < frames; i++) {
        SymFromAddr(process, (DWORD64)(stack[i]), 0, symbol);
        fprintf(f, "%d: %s - 0x%llx\n", frames - i - 1, symbol->Name, (unsigned long long)symbol->Address);
      }
      free(symbol);
    }
    SymCleanup(process);
    fclose(f);
  }

  if (showMessage) {
    std::string exceptionMessage = e.what();
    std::wstring message = L"An unexpected error occurred:\n\n";
    message += std::wstring(exceptionMessage.begin(), exceptionMessage.end());
    message += L"\n\nDetails have been written to the log directory. Please open an issue on GitHub if the problem persists.\n\nPress Ctrl+O in the Remote to restart Visualizer.";

    MessageBoxW(NULL, message.c_str(), L"MDropDX12 Error", MB_OK | MB_ICONERROR);
  }
}