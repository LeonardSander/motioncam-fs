#pragma once

#include <QSize>
#include <QString>

namespace motioncam {

bool validUiMode(const QString& mode);
bool useTouchUi(const QString& mode, bool touchscreen, const QSize& screen);
QString preferredUiMode();

// Select the layout once, before constructing application windows.
void initializeUiMode(const QString& mode);
bool touchUi();

} // namespace motioncam
