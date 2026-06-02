// SPDX-License-Identifier: MPL-2.0
// Minimal protocol peer for tests: no engine, just publishes one frame.
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QLocalSocket>
#include <QSharedMemory>
#include <QThread>
#include <cstring>

#include "RasterIpcProtocol.h"

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QCommandLineParser p;
  QCommandLineOption shm("shm", "shm key", "key");
  QCommandLineOption sock("sock", "socket name", "name");
  QCommandLineOption ww("width", "w", "w", "640");
  QCommandLineOption hh("height", "h", "h", "400");
  QCommandLineOption wad("wad", "asset path (ignored by stub)", "path");
  p.addOptions({shm, sock, ww, hh, wad});
  p.process(app);

  const uint32_t w = p.value(ww).toUInt();
  const uint32_t h = p.value(hh).toUInt();

  QSharedMemory mem(p.value(shm));
  QElapsedTimer attach_timer;
  attach_timer.start();
  while (!mem.attach() && attach_timer.elapsed() < 2000) {
    QThread::msleep(20);
  }
  if (!mem.isAttached()) {
    return 2;
  }
  QLocalSocket socket;
  socket.connectToServer(p.value(sock));
  if (!socket.waitForConnected(2000)) {
    return 3;
  }

  mem.lock();
  auto* base = static_cast<unsigned char*>(mem.data());
  auto* hdr = reinterpret_cast<pj_raster::ShmHeader*>(base);
  hdr->magic = pj_raster::kShmMagic;
  hdr->version = pj_raster::kShmVersion;
  hdr->width = w;
  hdr->height = h;
  hdr->bytes_per_pixel = pj_raster::kBytesPerPixel;
  hdr->buffer_count = pj_raster::kBufferCount;
  unsigned char* buf0 = base + pj_raster::bufferOffset(0, w, h, pj_raster::kBytesPerPixel);
  std::memset(buf0, 0x80, pj_raster::bufferBytes(w, h, pj_raster::kBytesPerPixel));
  hdr->active_index = 0;
  hdr->frame_seq = 1;
  mem.unlock();

  uint8_t msg[pj_raster::kMessageBytes];
  pj_raster::encodeMessage({pj_raster::MsgType::kFrameReady, 1}, msg);
  socket.write(reinterpret_cast<char*>(msg), pj_raster::kMessageBytes);
  socket.flush();

  QObject::connect(&socket, &QLocalSocket::readyRead, [&] {
    while (socket.bytesAvailable() >= pj_raster::kMessageBytes) {
      QByteArray b = socket.read(pj_raster::kMessageBytes);
      if (pj_raster::decodeMessage(reinterpret_cast<const uint8_t*>(b.constData())).type == pj_raster::MsgType::kBye) {
        app.quit();
      }
    }
  });
  QObject::connect(&socket, &QLocalSocket::disconnected, &app, &QCoreApplication::quit);
  return app.exec();
}
