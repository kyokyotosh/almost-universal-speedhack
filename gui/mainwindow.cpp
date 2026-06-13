#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "injector.h"

#include <QSignalBlocker>
#include <QMessageBox>
#include <QDateTime>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QCoreApplication>
#include <QIcon>
#include <QPixmap>
#include <QImage>

#include <shellapi.h>

#include <cmath>
#include <algorithm> 

// Keep windows.h from polluting the global namespace / clashing with Qt.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

// ---- speed mapping: slider position <-> multiplier (logarithmic) ----------
static constexpr double kMinMult = 0.1;
static constexpr double kMaxMult = 500.0;

static double sliderToMult(int pos)        // pos in [0..1000]
{
    const double t = pos / 1000.0;
    return kMinMult * std::pow(kMaxMult / kMinMult, t);
}

static int multToSlider(double mult)
{
    mult = std::clamp(mult, kMinMult, kMaxMult);
    const double t = std::log(mult / kMinMult) / std::log(kMaxMult / kMinMult);
    return static_cast<int>(std::lround(t * 1000.0));
}

// ---------------------------------------------------------------------------
// Free helper: best-effort architecture of a process ("x86" / "x64" / "?").
// ---------------------------------------------------------------------------
static QString processArch(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return QStringLiteral("?");

    QString arch = QStringLiteral("?");
    USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
    USHORT nativeMachine  = IMAGE_FILE_MACHINE_UNKNOWN;

    if (IsWow64Process2(h, &processMachine, &nativeMachine)) {
        if (processMachine == IMAGE_FILE_MACHINE_UNKNOWN) {
            // Running natively -> matches the OS architecture.
            arch = (nativeMachine == IMAGE_FILE_MACHINE_AMD64)
                       ? QStringLiteral("x64")
                       : QStringLiteral("x86");
        } else {
            // Under WOW64 -> 32-bit process on a 64-bit OS.
            arch = QStringLiteral("x86");
        }
    }

    CloseHandle(h);
    return arch;
}

// PID -> full path of the backing executable.
static QString processPath(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH];
    DWORD sz = MAX_PATH;
    QString path;
    if (QueryFullProcessImageNameW(h, 0, buf, &sz))
        path = QString::fromWCharArray(buf, sz);
    CloseHandle(h);
    return path;
}

// Shell icon for an exe path (matches what Explorer shows). Empty QIcon on failure.
static QIcon extractIcon(const QString& path)
{
    if (path.isEmpty()) return {};
    SHFILEINFOW info{};
    if (SHGetFileInfoW(reinterpret_cast<const wchar_t*>(path.utf16()), 0,
                       &info, sizeof(info), SHGFI_ICON | SHGFI_SMALLICON)
        && info.hIcon) {
        QImage img = QImage::fromHICON(info.hIcon);   // Qt 6.5+ built-in
        DestroyIcon(info.hIcon);
        return QIcon(QPixmap::fromImage(img));
    }
    return {};
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    ui->processTable->setColumnWidth(0, 70);   // PID
    ui->processTable->setColumnWidth(1, 280);  // Process
    ui->processTable->setIconSize(QSize(16, 16));

    Injector::enableDebugPrivilege();

    wireConnections();
    registerHotkeys();

    setInjectedState(false);
    refreshProcessList();
    log(QStringLiteral("Ready."));
}

MainWindow::~MainWindow()
{
    unregisterHotkeys();
    delete ui;
}

// ---------------------------------------------------------------------------
// Signal/slot wiring — all in one place so it's auditable.
// ---------------------------------------------------------------------------
void MainWindow::wireConnections()
{
    connect(ui->refreshButton, &QPushButton::clicked,
            this, &MainWindow::refreshProcessList);

    connect(ui->filterEdit, &QLineEdit::textChanged,
            this, &MainWindow::filterProcessList);

    connect(ui->processTable, &QTableWidget::itemSelectionChanged,
            this, &MainWindow::onProcessSelectionChanged);

    connect(ui->injectButton, &QPushButton::clicked, this, &MainWindow::onInject);
    connect(ui->ejectButton,  &QPushButton::clicked, this, &MainWindow::onEject);

    connect(ui->enableCheck, &QCheckBox::toggled,
            this, &MainWindow::onEnableToggled);

    connect(ui->speedSlider, &QSlider::valueChanged,
            this, &MainWindow::onSliderChanged);
    connect(ui->speedSpin, qOverload<double>(&QDoubleSpinBox::valueChanged),
            this, &MainWindow::onSpinChanged);

    // Presets + reset all funnel through setSpeed().
    connect(ui->preset05, &QPushButton::clicked, this, [this]{ setSpeed(0.5); });
    connect(ui->preset1,  &QPushButton::clicked, this, [this]{ setSpeed(1.0); });
    connect(ui->preset2,  &QPushButton::clicked, this, [this]{ setSpeed(2.0); });
    connect(ui->preset4,  &QPushButton::clicked, this, [this]{ setSpeed(4.0); });
    connect(ui->resetButton, &QPushButton::clicked, this, [this]{ setSpeed(1.0); });
}

// ---------------------------------------------------------------------------
// Process enumeration
// ---------------------------------------------------------------------------
void MainWindow::refreshProcessList()
{
    ui->processTable->setRowCount(0);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        log(QStringLiteral("Snapshot failed."));
        return;
    }

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(snap, &pe)) {
        do {
            const int row = ui->processTable->rowCount();
            ui->processTable->insertRow(row);

            auto *pidItem  = new QTableWidgetItem(QString::number(pe.th32ProcessID));
            auto *nameItem = new QTableWidgetItem(QString::fromWCharArray(pe.szExeFile));

            const QString path = processPath(pe.th32ProcessID);
            QIcon icon;
            auto it = m_iconCache.constFind(path);          // already seen this exe?
            if (it != m_iconCache.constEnd()) {
                icon = it.value();
            } else {
                icon = extractIcon(path);
                m_iconCache.insert(path, icon);             // cache even empties
            }
            nameItem->setIcon(icon);

            auto *archItem = new QTableWidgetItem(processArch(pe.th32ProcessID));

            // Stash the real PID on the row so selection doesn't depend on parsing text.
            pidItem->setData(Qt::UserRole, QVariant::fromValue<quint32>(pe.th32ProcessID));

            ui->processTable->setItem(row, 0, pidItem);
            ui->processTable->setItem(row, 1, nameItem);
            ui->processTable->setItem(row, 2, archItem);
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);

    // Re-apply any active filter to the freshly rebuilt list.
    filterProcessList(ui->filterEdit->text());
    log(QStringLiteral("Process list refreshed (%1 entries).")
            .arg(ui->processTable->rowCount()));
}

void MainWindow::filterProcessList(const QString &text)
{
    const QString needle = text.trimmed();
    for (int row = 0; row < ui->processTable->rowCount(); ++row) {
        const QString name = ui->processTable->item(row, 1)->text();
        const bool match = needle.isEmpty()
                           || name.contains(needle, Qt::CaseInsensitive);
        ui->processTable->setRowHidden(row, !match);
    }
}

void MainWindow::onProcessSelectionChanged()
{
    const auto rows = ui->processTable->selectionModel()->selectedRows();
    if (rows.isEmpty()) {
        m_targetPid = 0;
        ui->injectButton->setEnabled(false);
        return;
    }

    const int row = rows.first().row();
    m_targetPid = ui->processTable->item(row, 0)
                      ->data(Qt::UserRole).value<quint32>();

    // Can only inject when something is selected and we're not already in.
    ui->injectButton->setEnabled(!m_injected && m_targetPid != 0);
}

// ---------------------------------------------------------------------------
// Inject / eject  (the dangerous parts live behind these stubs)
// ---------------------------------------------------------------------------
void MainWindow::onInject()
{
    if (m_targetPid == 0)
        return;

    // Refuse a bitness mismatch up front — clearer than LoadLibrary failing later.
    const QString ownArch = (sizeof(void*) == 8) ? "x64" : "x86";
    const int row = ui->processTable->selectionModel()->selectedRows().first().row();
    const QString targetArch = ui->processTable->item(row, 2)->text();
    if (targetArch != ownArch && targetArch != "?") {
        log(QStringLiteral("Bitness mismatch: target is %1, injector is %2. "
                           "Build a %1 version to inject this game.")
                .arg(targetArch, ownArch));
        return;
    }

    const QString dll = QCoreApplication::applicationDirPath() + "/speedhack.dll";
    log(QStringLiteral("Injecting into PID %1…").arg(m_targetPid));

    InjectResult r = Injector::inject(m_targetPid, dll.toStdWString());
    log(QString::fromStdWString(r.message));
    if (!r.ok)
        return;                                   // stay un-injected on failure

    if (m_control.open(m_targetPid))
        log(QStringLiteral("Control channel connected."));
    else
        log(QStringLiteral("Injected, but control channel failed."));

    setInjectedState(true);
    applySpeedToTarget(m_speed);
}

void MainWindow::onEject()
{
    log(QStringLiteral("Ejecting…"));

    // TODO: signal the DLL to restore real timing (multiplier = 1.0) and
    //   FreeLibrary it, or just reset to 1.0 and detach the IPC handle.

    m_control.setSpeed(1.0);   // restore real time before detaching
    m_control.close();

    setInjectedState(false);
}

// ---------------------------------------------------------------------------
// Speed control — setSpeed() is the single source of truth.
// ---------------------------------------------------------------------------
void MainWindow::onEnableToggled(bool enabled)
{
    // Enabling resumes the chosen multiplier; disabling forces 1.0 without
    // losing the slider position the user picked.
    applySpeedToTarget(enabled ? m_speed : 1.0);
    log(enabled ? QStringLiteral("Speedhack enabled.")
                : QStringLiteral("Speedhack disabled."));
}

void MainWindow::onSliderChanged(int value)
{
    setSpeed(sliderToMult(value));
}

void MainWindow::onSpinChanged(double value)
{
    setSpeed(value);
}

void MainWindow::setSpeed(double multiplier)
{
    m_speed = multiplier;
    {
        QSignalBlocker bSlider(ui->speedSlider);
        QSignalBlocker bSpin(ui->speedSpin);
        ui->speedSlider->setValue(multToSlider(multiplier));   // was multiplier*100
        ui->speedSpin->setValue(multiplier);
    }
    ui->activeSpeedLabel->setText(QStringLiteral("%1×").arg(multiplier, 0, 'f', 2));
    if (ui->enableCheck->isChecked())
        applySpeedToTarget(multiplier);
}


void MainWindow::cyclePreset()
{
    static const double presets[] = { 0.5, 1.0, 2.0, 4.0 };
    constexpr int n = sizeof(presets) / sizeof(presets[0]);

    int idx = 0;
    for (int i = 0; i < n; ++i)
        if (qFuzzyCompare(presets[i], m_speed)) { idx = i; break; }

    setSpeed(presets[(idx + 1) % n]);
}

void MainWindow::applySpeedToTarget(double m)
{
    if (m_control.valid())
        m_control.setSpeed(m);
}

// ---------------------------------------------------------------------------
// UI state
// ---------------------------------------------------------------------------
void MainWindow::setInjectedState(bool injected)
{
    m_injected = injected;

    ui->statusLabel->setText(injected
        ? QStringLiteral("Injected into PID %1").arg(m_targetPid)
        : QStringLiteral("Not injected"));

    ui->ejectButton->setEnabled(injected);
    ui->injectButton->setEnabled(!injected && m_targetPid != 0);

    // Lock target selection while attached.
    ui->processTable->setEnabled(!injected);
    ui->refreshButton->setEnabled(!injected);
    ui->filterEdit->setEnabled(!injected);
}

void MainWindow::log(const QString &msg)
{
    const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    ui->logView->appendPlainText(QStringLiteral("[%1] %2").arg(ts, msg));
}

// ---------------------------------------------------------------------------
// Global hotkeys
// ---------------------------------------------------------------------------
void MainWindow::registerHotkeys()
{
    // winId() forces native window creation, giving us a valid, stable HWND.
    const HWND hwnd = reinterpret_cast<HWND>(winId());

    const UINT mod = MOD_CONTROL | MOD_ALT;
    bool ok = true;
    ok &= RegisterHotKey(hwnd, HotkeyToggle, mod, VK_HOME);  // Ctrl+Alt+Home
    ok &= RegisterHotKey(hwnd, HotkeyReset,  mod, VK_END);   // Ctrl+Alt+End
    ok &= RegisterHotKey(hwnd, HotkeyCycle,  mod, VK_NEXT);  // Ctrl+Alt+PageDown

    if (!ok)
        log(QStringLiteral("Warning: one or more hotkeys are already in use."));
}

void MainWindow::unregisterHotkeys()
{
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    UnregisterHotKey(hwnd, HotkeyToggle);
    UnregisterHotKey(hwnd, HotkeyReset);
    UnregisterHotKey(hwnd, HotkeyCycle);
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    if (eventType == "windows_generic_MSG") {
        MSG *msg = static_cast<MSG *>(message);
        if (msg->message == WM_HOTKEY) {
            switch (msg->wParam) {
            case HotkeyToggle:
                ui->enableCheck->toggle();   // fires onEnableToggled
                return true;
            case HotkeyReset:
                setSpeed(1.0);
                return true;
            case HotkeyCycle:
                cyclePreset();
                return true;
            default:
                break;
            }
        }
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}