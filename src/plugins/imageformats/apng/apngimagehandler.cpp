#include "apngimagehandler_p.h"
#include <qcolor.h>
#include <qimage.h>
#include <qdebug.h>
#include <qpainter.h>
#include <qvariant.h>

static const int pngHeaderSize = 8;

QApngHandler::QApngHandler()
{

}

QApngHandler::~QApngHandler()
{
    if (png_ptr && info_ptr)
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);

    delete m_composited;
}

bool QApngHandler::canRead() const
{
    if (m_scanState == ScanNotScanned && !canRead(device()))
        return false;

    if (m_scanState != ScanError) {
        if (m_hasAnimation) {
            if (m_frameInfo.frame_num >= m_frameCount)
                return false;
        } else {
            if (m_frameInfo.frame_num >= 1)
                return false;
        }

        setFormat(m_hasAnimation ? "apng" : "png");
        return true;
    }
    return false;
}

bool QApngHandler::canRead(QIODevice *device)
{
    if (!device) {
        qWarning("QApngHandler::canRead() called with no device");
        return false;
    }

    auto sig = device->peek(pngHeaderSize);
    return png_sig_cmp(reinterpret_cast<png_const_bytep>(sig.constData()), 0, static_cast<png_size_t>(sig.size())) == 0;
}

void QApngHandler::readCallback(png_structp png_ptr, png_byte* raw_data, png_size_t read_length)
{
    QApngHandler* handle = (QApngHandler*)png_get_io_ptr(png_ptr);

    if (handle->m_rawDataReadIndex + read_length < (png_size_t)handle->m_rawData.size()) {
        memcpy(raw_data, handle->m_rawData.data() + handle->m_rawDataReadIndex, read_length);
        handle->m_rawDataReadIndex += read_length;
    }
}

bool QApngHandler::ensureScanned() const
{
    if (m_scanState != ScanNotScanned)
        return m_scanState == ScanSuccess;

    m_scanState = ScanError;

    if (device()->isSequential()) {
        qWarning() << "Sequential devices are not supported";
        return false;
    }

    qint64 oldPos = device()->pos();
    device()->seek(0);

    QApngHandler *that = const_cast<QApngHandler *>(this);
    if (!that->canRead(device()))
        return false;

    that->ensureDemuxer();

    that->png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    that->info_ptr = png_create_info_struct(png_ptr);
    if (!that->png_ptr || !that->info_ptr) {
        qCritical() << "failed to create apng struct";
        return false;
    }

    png_set_read_fn(png_ptr, (png_voidp)this, QApngHandler::readCallback);

    if (setjmp(png_jmpbuf(png_ptr)))
        return false;
    png_set_sig_bytes(png_ptr, pngHeaderSize);
    that->m_rawDataReadIndex += pngHeaderSize;
    png_read_info(png_ptr, info_ptr);
    png_set_expand(png_ptr);
    png_set_strip_16(png_ptr);
    png_set_gray_to_rgb(png_ptr);
    png_set_add_alpha(png_ptr, 0xff, PNG_FILLER_AFTER);
    png_set_bgr(png_ptr);
    (void)png_set_interlace_handling(png_ptr);
    png_read_update_info(png_ptr, info_ptr);

    that->m_imageSize = QSize(png_get_image_width(png_ptr, info_ptr), png_get_image_height(png_ptr, info_ptr));

    that->m_loop = 0;
    that->m_frameInfo.delay_num = 1;
    that->m_frameInfo.delay_den = 10;
    that->m_frameInfo.bop = 0;
    that->m_frameInfo.dop = 0;
    that->m_frameInfo.width = m_imageSize.width();
    that->m_frameInfo.height = m_imageSize.height();
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_acTL)) {
        png_get_acTL(png_ptr, info_ptr, (png_uint_32 *)&that->m_frameCount, (png_uint_32 *)&that->m_loop);
        that->m_hasAnimation = true;
        that->m_skipFirst = png_get_first_frame_is_hidden(png_ptr, info_ptr);
        that->m_composited = new QImage(that->m_imageSize.width(), that->m_imageSize.height(), QImage::Format_ARGB32);
        that->m_composited->fill(Qt::transparent);
    }

    device()->seek(oldPos);

    m_scanState = ScanSuccess;
    return true;
}

bool QApngHandler::ensureDemuxer()
{
    if (!m_rawData.isEmpty())
        return true;

    m_rawData = device()->readAll();
    return true;
}

bool QApngHandler::read(QImage *image)
{
    if (!ensureScanned() || device()->isSequential() || !ensureDemuxer())
        return false;

    QRect prevFrameRect;
    if (m_frameInfo.frame_num != 0 && m_frameInfo.dop == PNG_DISPOSE_OP_BACKGROUND)
        prevFrameRect = currentImageRect();

    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_acTL))
    {
        png_read_frame_head(png_ptr, info_ptr);
        png_get_next_frame_fcTL(png_ptr,
                                info_ptr,
                                &m_frameInfo.width,
                                &m_frameInfo.height,
                                &m_frameInfo.x,
                                &m_frameInfo.y,
                                &m_frameInfo.delay_num,
                                &m_frameInfo.delay_den,
                                &m_frameInfo.dop,
                                &m_frameInfo.bop);
    }

    if (m_frameInfo.frame_num == 0 || (m_frameInfo.frame_num == 1 && m_skipFirst))
    {
        m_frameInfo.bop = PNG_BLEND_OP_SOURCE;
        if (m_frameInfo.dop == PNG_DISPOSE_OP_PREVIOUS)
            m_frameInfo.dop = PNG_DISPOSE_OP_BACKGROUND;
    }

    QImage frame(m_frameInfo.width, m_frameInfo.height, QImage::Format_ARGB32);

    png_bytepp rows_frame = (png_bytepp)malloc(m_frameInfo.height * sizeof(png_bytep));
    auto lineSize = m_frameInfo.width * 4;
    for (png_uint_32 i = 0; i < m_frameInfo.height; i++) {
        rows_frame[i] = frame.bits() + i * (lineSize);
    }
    png_read_image(png_ptr, rows_frame);
    free(rows_frame);

    if (!m_hasAnimation)
        *image = frame;
    else {
        QPainter painter(m_composited);
        if (!prevFrameRect.isEmpty()) {
            painter.setCompositionMode(QPainter::CompositionMode_Clear);
            painter.fillRect(prevFrameRect, Qt::black);
        }

        if (m_frameInfo.bop == PNG_BLEND_OP_OVER)
            painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        else
            painter.setCompositionMode(QPainter::CompositionMode_Source);

        painter.drawImage(currentImageRect(), frame);

        *image = *m_composited;
    }

    m_frameInfo.frame_num++;
    return true;
}


QVariant QApngHandler::option(ImageOption option) const
{
    if (!supportsOption(option) || !ensureScanned())
        return QVariant();

    switch (option) {
    case Size:
        return m_imageSize;
    case Animation:
        return m_hasAnimation;
    default:
        return QVariant();
    }
}

bool QApngHandler::supportsOption(ImageOption option) const
{
    return option == Size || option == Animation;
}

#if QT_DEPRECATED_SINCE(5, 13)
QByteArray QApngHandler::name() const
{
    return QByteArrayLiteral("apng");
}
#endif

int QApngHandler::imageCount() const
{
    if (!ensureScanned())
        return 0;

    if (!m_hasAnimation)
        return 1;

    return m_frameCount;
}

int QApngHandler::currentImageNumber() const
{
    if (!ensureScanned() || !m_hasAnimation)
        return 0;

    return m_frameInfo.frame_num;
}

QRect QApngHandler::currentImageRect() const
{
    if (!ensureScanned())
        return QRect();

    return QRect(m_frameInfo.x, m_frameInfo.y, m_frameInfo.width, m_frameInfo.height);
}

int QApngHandler::loopCount() const
{
    if (!ensureScanned() || !m_hasAnimation)
        return 0;

    return m_loop == 0 ? -1 : m_loop;
}

int QApngHandler::nextImageDelay() const
{
    if (!ensureScanned() || !m_hasAnimation)
        return 0;

    auto delay = static_cast<double>(m_frameInfo.delay_num) / static_cast<double>(m_frameInfo.delay_den);
    return qRound(delay * 1000);
}
