#ifndef APNGIMAGEHANDLER_P_H
#define APNGIMAGEHANDLER_P_H

#include <QtGui/qcolor.h>
#include <QtGui/qcolorspace.h>
#include <QtGui/qimage.h>
#include <QtGui/qimageiohandler.h>
#include <QtCore/qbytearray.h>
#include <QtCore/qsize.h>

#include <png.h>

#ifndef PNG_APNG_SUPPORTED
#error libpng with APNG patch is required
#endif
#ifndef PNG_READ_APNG_SUPPORTED
#error libpng with APNG patch and APNG read support is required
#endif


class QApngHandler : public QImageIOHandler
{
public:
    QApngHandler();
    ~QApngHandler();

public:
#if QT_DEPRECATED_SINCE(5, 13)
    QByteArray name() const override;
#endif

    bool canRead() const override;
    bool read(QImage *image) override;

    static bool canRead(QIODevice *device);

    QVariant option(ImageOption option) const override;
    bool supportsOption(ImageOption option) const override;

    int imageCount() const override;
    int currentImageNumber() const override;
    QRect currentImageRect() const override;
    int loopCount() const override;
    int nextImageDelay() const override;

    static void readCallback(png_structp png_ptr, png_byte* raw_data, png_size_t read_length);

private:
    bool ensureScanned() const;
    bool ensureDemuxer();

private:
    enum ScanState {
        ScanError = -1,
        ScanNotScanned = 0,
        ScanSuccess = 1,
    };

    struct ApngFrameInfo {
        png_uint_32 x = 0;
        png_uint_32 y = 0;
        png_uint_32 width = 0;
        png_uint_32 height = 0;

        png_uint_16 delay_num = 0;
        png_uint_16 delay_den = 1;
        quint8 dop = PNG_DISPOSE_OP_NONE;
        quint8 bop = PNG_BLEND_OP_SOURCE;

        int frame_num = 0;
    } m_frameInfo;

    bool m_hasAnimation = false;
    bool m_skipFirst = false;
    mutable ScanState m_scanState = ScanNotScanned;
    QSize m_imageSize;
    int m_loop{};
    int m_frameCount{};
    png_size_t m_rawDataReadIndex = 0;
    QByteArray m_rawData;
    png_structp png_ptr{};
    png_infop   info_ptr{};
    QImage *m_composited{};   // For animation frames composition
};

#endif // APNGIMAGEHANDLER_P_H
