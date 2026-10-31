#include "GalleryVulkanVideo.h"
#include <QPainter>
#include <QApplication>
#include <QListView>
#include <QStandardItemModel>
#include <QScrollBar>
#include <QLabel>
#include <cstdio>

class Patch final : public QWidget {
public:
    Patch(QWidget* parent, QColor color) : QWidget(parent), mColor(color) {}
private:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), mColor);
        painter.fillRect(QRect(width() / 4, height() / 4, 7, 5), Qt::white);
    }
    QColor mColor;
};

class MarkerSlider final : public QSlider {
public:
    explicit MarkerSlider(QWidget* parent):QSlider(Qt::Horizontal,parent) {}
    void setMarker(int x){mMarker=x;invalidateGalleryOverlay(*this);update();}
protected:
    void paintEvent(QPaintEvent* event) override {
        QSlider::paintEvent(event);
        QPainter painter(this);painter.fillRect(mMarker,height()-4,5,4,Qt::red);
    }
private:
    int mMarker=0;
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QWidget stage;
    stage.resize(800, 600);
    QWidget viewport(&stage);
    viewport.setGeometry(13, 17, 760, 560);
    QWidget overlay(&stage);
    overlay.setGeometry(7, 9, 780, 580);
    Patch title(&overlay, QColor(170, 20, 30, 180));
    Patch histogram(&overlay, QColor(20, 170, 30, 140));
    Patch controls(&overlay, QColor(20, 30, 170, 200));
    Patch slider(&overlay, QColor(170, 80, 30, 200));
    Patch fps(&stage, QColor(80, 80, 80, 210));
    title.setGeometry(22, 30, 170, 25);
    histogram.setGeometry(22, 65, 200, 80);
    controls.setGeometry(270, 430, 240, 38);
    slider.setGeometry(0, 510, 780, 28);
    fps.setGeometry(650, 28, 100, 25);
    stage.show();
    QApplication::processEvents();
    for (const QSize size : {QSize(760, 560), QSize(490, 350)}) {
        viewport.resize(size);
        for (bool histogramVisible : {false, true}) {
            histogram.setVisible(histogramVisible);
            for (bool childrenOnly : {false, true})
            for (bool excluded : {false, true})
            for (qreal ratio : {1.0, 1.25, 1.5, 2.0, 4.0}) {
                for (qreal opacity : {1.0, 0.4, 0.0}) {
                    std::vector<GalleryOverlayRoot> roots{{&overlay, opacity, excluded ? QRegion(histogram.geometry()) : QRegion(), childrenOnly}, {&fps, 1.0}};
                    if(excluded&&histogramVisible&&ratio==4.0)
                        roots.push_back({&histogram,opacity});
                    const QSize pixels(int(std::ceil(size.width() * ratio)), int(std::ceil(size.height() * ratio)));
                    QImage expected(pixels, QImage::Format_RGBA8888_Premultiplied);
                    expected.fill(Qt::transparent);
                    {
                        QPainter painter(&expected);
                        painter.scale(ratio, ratio);
                        for (const auto& root : roots) {
                            if (root.opacity <= 0.0) continue;
                            painter.setOpacity(root.opacity);
                            const QPoint position=root.widget->mapTo(&stage,QPoint())-viewport.pos();
                                const QRegion region=QRegion(root.widget->rect())-root.excluded;
                                root.widget->render(&painter, position + region.boundingRect().topLeft(),
                                                    region, QWidget::DrawChildren);
                        }
                    }
                    // Native presentation captures widgets explicitly while their
                    // ordinary backing-store updates remain disabled.
                    overlay.setUpdatesEnabled(false);
                    fps.setUpdatesEnabled(false);
                    auto captured = captureGalleryOverlay(viewport, roots, ratio);
                    if(overlay.updatesEnabled()||fps.updatesEnabled()||title.updatesEnabled()) {
                        std::fprintf(stderr,"Capture enabled backing-store updates\n");
                        return 1;
                    }
                    overlay.setUpdatesEnabled(true);
                    fps.setUpdatesEnabled(true);
                    QImage actual(pixels, expected.format());
                    actual.fill(Qt::transparent);
                    {
                        QPainter painter(&actual);
                        for (const auto& part : captured.parts)
                            painter.drawImage(part.target, captured.image, part.source);
                    }
                    if (actual != expected) {
                        int differences = 0;
                        for (int y = 0; y < actual.height(); ++y)
                            for (int x = 0; x < actual.width(); ++x)
                                if (actual.pixel(x, y) != expected.pixel(x, y)) {
                                    if (differences == 0) std::fprintf(stderr, "First mismatch %d,%d: %08x vs %08x\n", x, y, actual.pixel(x,y), expected.pixel(x,y));
                                    ++differences;
                                }
                        std::fprintf(stderr, "Overlay mismatch: %dx%d ratio=%.2f opacity=%.1f histogram=%d pixels=%d\n",
                            size.width(), size.height(), ratio, opacity, histogramVisible, differences);
                        return 1;
                    }
                    if (size.height() == 560 && opacity > 0 && captured.image.height() >= pixels.height()) {
                        std::fprintf(stderr, "Overlay was not compacted\n");
                        return 1;
                    }
                }
            }
        }
    }
    {
        QWidget host;
        host.resize(460,180);
        QListView view(&host);
        view.setGeometry(11,13,420,140);
        view.setViewMode(QListView::IconMode);view.setFlow(QListView::LeftToRight);
        view.setWrapping(false);view.setMovement(QListView::Static);
        view.setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        view.setUniformItemSizes(true);
        view.setStyleSheet("QListView{background:rgba(0,0,0,80);border:0;}");
        QStandardItemModel model;
        for(int i=0;i<9;++i){
            auto* item=new QStandardItem(QString::number(i));
            item->setCheckable(true);item->setSizeHint(QSize(96,110));
            QPixmap icon(64,48);icon.fill(QColor(30+i*20,50,80,180));item->setIcon(QIcon(icon));
            model.appendRow(item);
        }
        view.setModel(&model);
        QLabel caption("caption",&host);caption.setGeometry(11,155,200,20);
        host.show();QApplication::processEvents();
        GalleryOverlayCache cache;
        for(qreal ratio:{1.0,1.25,4.0})for(int value:{0,117,243,0})for(bool selected:{false,true}){
            model.item(2)->setCheckState(selected?Qt::Checked:Qt::Unchecked);
            caption.setText(selected?"selected":"caption");
            view.horizontalScrollBar()->setValue(value);
            QApplication::processEvents();
            QImage expected(host.size()*ratio,QImage::Format_RGBA8888_Premultiplied);
            expected.fill(Qt::transparent);
            {QPainter painter(&expected);painter.scale(ratio,ratio);
             view.render(&painter,view.pos(),QRegion(),QWidget::DrawChildren);
             caption.render(&painter,caption.pos(),QRegion(),QWidget::DrawChildren);}
            host.setUpdatesEnabled(false);
            auto captured=captureGalleryOverlay(host,{{&view,1.0},{&caption,1.0}},ratio,&cache);
            if(host.updatesEnabled()||view.updatesEnabled()||caption.updatesEnabled())return 1;
            host.setUpdatesEnabled(true);
            QImage actual(expected.size(),expected.format());actual.fill(Qt::transparent);
            {QPainter painter(&actual);for(const auto& part:captured.parts)
                painter.drawImage(part.target,captured.image,part.source);}
            int count=0;
            for(int y=0;y<actual.height();++y)for(int x=0;x<actual.width();++x){
                const QRgb a=actual.pixel(x,y),b=expected.pixel(x,y);
                // Cached glyphs add a premultiplied-alpha composition step.
                count+=std::max({std::abs(qRed(a)-qRed(b)),std::abs(qGreen(a)-qGreen(b)),
                    std::abs(qBlue(a)-qBlue(b)),std::abs(qAlpha(a)-qAlpha(b))})>2;
            }
            if(count){
                std::fprintf(stderr,"Item-view overlay differs ratio=%g scroll=%d checked=%d pixels=%d\n",double(ratio),value,selected,count);
                return 1;
            }
        }
        model.setRowCount(20000);view.doItemsLayout();
        view.horizontalScrollBar()->setValue(500);
        QApplication::processEvents();
        auto captured=captureGalleryOverlay(host,{{&view,1.0}},4.0,&cache);
        if(captured.image.width()>host.width()*4||captured.image.height()>host.height()*4)return 1;
    }
    {
        QWidget host;host.resize(340,80);
        MarkerSlider slider(&host);slider.setGeometry(10,10,320,40);
        slider.setRange(0,99);slider.setValue(0);
        host.show();QApplication::processEvents();host.setUpdatesEnabled(false);
        GalleryOverlayCache cache;
        for(qreal ratio:{1.0,1.25,4.0}){
            slider.setMarker(30);
            auto first=captureGalleryOverlay(host,{{&slider,1.0}},ratio,&cache);
            auto warm=captureGalleryOverlay(host,{{&slider,1.0}},ratio,&cache);
            if(warm.reusedWidgets!=1||warm.image!=first.image)return 1;
            // Custom paint data changes while value/range stay fixed, as when
            // a paused gallery switches between equal-length clips.
            slider.setMarker(220);
            auto changed=captureGalleryOverlay(host,{{&slider,1.0}},ratio,&cache);
            auto fresh=captureGalleryOverlay(host,{{&slider,1.0}},ratio);
            if(changed.image==first.image||changed.image!=fresh.image||changed.paintedWidgets!=1)return 1;
        }
    }
    return 0;
}
