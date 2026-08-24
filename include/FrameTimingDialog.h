#pragma once

#include <QDialog>
#include <cstdint>
#include <vector>

class FrameTimingDialog final : public QDialog
{
public:
    FrameTimingDialog(const QString& clipName,
                      const std::vector<std::int64_t>& pts,
                      int timeBaseNum,
                      int timeBaseDen,
                      double targetFps,
                      bool useCfrMapping,
                      QWidget* parent = nullptr);

    void updateTiming(const std::vector<std::int64_t>& pts,
                      int timeBaseNum,
                      int timeBaseDen,
                      double targetFps,
                      bool useCfrMapping);

private:
    class QLabel* mSummary = nullptr;
    class QWidget* mGraph = nullptr;
    std::uint64_t mAnalysisGeneration = 0;
};
