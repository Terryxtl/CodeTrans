#include "qr_fallback.h"
#include "ReadBarcode.h"

void cp_find_qr(const uint8_t *y, int width, int height, int stride,
                cp_qr_found found, void *user)
{
    try {
        ZXing::ImageView image(y, width, height, ZXing::ImageFormat::Lum, stride);
        auto options = ZXing::ReaderOptions().formats(ZXing::BarcodeFormat::QRCode)
            .tryHarder(true).tryRotate(false).tryInvert(false).tryDownscale(true);
        for (const auto& qr : ZXing::ReadBarcodes(image, options)) {
            if (!qr.isValid() || qr.bytes().empty() || qr.bytes().size() > QUIRC_MAX_PAYLOAD)
                continue;
            cp_grid grid{};
            grid.size = qr.symbol().width();
            for (int k = 0; k < 4; ++k)
                grid.corners[k] = {qr.position()[k].x, qr.position()[k].y};
            found(user, &grid, qr.bytes().data(), qr.bytes().size());
        }
    } catch (const std::exception&) {
        // A failed optional pass leaves the quirc results intact.
    }
}
