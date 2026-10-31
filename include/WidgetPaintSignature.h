#pragma once
#include <QWidget>
#include <QLayout>
#include <QAbstractButton>
#include <QAbstractSlider>
#include <QLabel>
#include <QComboBox>
#include <QLineEdit>
#include <QProgressBar>
#include <QVariant>

namespace motioncam {
// Root position is deliberately excluded: moving a widget reuses its raster.
inline quint64 widgetPaintSignature(QWidget& root){
    quint64 signature=0;
    const auto mix=[&](quint64 value){signature^=value+0x9e3779b97f4a7c15ULL+(signature<<6)+(signature>>2);};
    auto widgets=root.findChildren<QWidget*>();widgets.prepend(&root);
    for(auto* child:widgets){
        child->ensurePolished();if(child->layout())child->layout()->activate();
        mix(qHash(child->size()));if(child!=&root)mix(qHash(child->pos()));
        mix(qHash(child->styleSheet()));mix(child->palette().cacheKey());
        mix(qHash(child->font().key()));mix(child->isVisible());mix(child->isEnabled());
        mix(child->underMouse());mix(child->hasFocus());
        mix(child->property("motioncamGalleryOverlayRevision").toULongLong());
        if(auto* button=qobject_cast<QAbstractButton*>(child)){
            mix(button->isDown());mix(button->isChecked());mix(qHash(button->text()));
            mix(button->icon().cacheKey());mix(qHash(button->iconSize()));
        }
        if(auto* slider=qobject_cast<QAbstractSlider*>(child)){
            mix(slider->value());mix(slider->minimum());mix(slider->maximum());
            mix(slider->isSliderDown());mix(slider->pageStep());
        }
        if(auto* label=qobject_cast<QLabel*>(child)){
            mix(qHash(label->text()));
#if QT_VERSION >= QT_VERSION_CHECK(6,0,0)
            mix(label->pixmap().cacheKey());
#else
            mix(label->pixmap()?label->pixmap()->cacheKey():0);
#endif
        }
        if(auto* combo=qobject_cast<QComboBox*>(child)){mix(combo->currentIndex());mix(qHash(combo->currentText()));}
        if(auto* edit=qobject_cast<QLineEdit*>(child)){mix(qHash(edit->text()));mix(qHash(edit->placeholderText()));}
        if(auto* progress=qobject_cast<QProgressBar*>(child)){mix(progress->value());mix(progress->minimum());mix(progress->maximum());}
    }
    return signature;
}
} // namespace motioncam
