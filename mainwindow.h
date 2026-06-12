#pragma once

#include <QMainWindow>
#include "speedhack_ipc.h"

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

private slots:
    void refreshProcessList();
    void filterProcessList(const QString &text);
    void onProcessSelectionChanged();
    void onInject();
    void onEject();
    void onEnableToggled(bool enabled);
    void onSliderChanged(int value);     // slider is in centi-units (value/100.0)
    void onSpinChanged(double value);

private:
    // RegisterHotKey IDs — matched in nativeEvent's WM_HOTKEY switch.
    enum HotkeyId { HotkeyToggle = 1, HotkeyReset = 2, HotkeyCycle = 3 };

    void wireConnections();
    void registerHotkeys();
    void unregisterHotkeys();

    void setSpeed(double multiplier);    // single source of truth for the speed value
    void applySpeedToTarget(double m);   // IPC stub -> shared memory
    void cyclePreset();
    void setInjectedState(bool injected);
    void log(const QString &msg);

    shipc::ControlWriter m_control;
    Ui::MainWindow *ui;


    quint32 m_targetPid = 0;
    bool    m_injected  = false;
    double  m_speed     = 1.0;
};