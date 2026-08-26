// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/updatepanel.h>

#include <qt/updatecontroller.h>
#include <update/manager.h>

#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

using update::UpdateState;

UpdatePanel::UpdatePanel(UpdateController* controller, QWidget* parent)
    : QWidget{parent}, m_controller{controller}
{
    auto* box{new QGroupBox{tr("B3 Hive update"), this}};
    auto* form{new QFormLayout};
    m_installed = new QLabel{this};
    m_latest = new QLabel{tr("—"), this};
    m_notes = new QLabel{tr("—"), this};
    m_notes->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_size = new QLabel{tr("—"), this};
    m_status = new QLabel{this};
    form->addRow(tr("Installed version:"), m_installed);
    form->addRow(tr("Latest verified version:"), m_latest);
    form->addRow(tr("Release notes digest:"), m_notes);
    form->addRow(tr("Download size:"), m_size);
    form->addRow(tr("Status:"), m_status);

    m_check = new QPushButton{tr("Check now"), this};
    m_download = new QPushButton{tr("Download update"), this};
    m_install = new QPushButton{tr("Automatic install unavailable"), this};
    m_install_note = new QLabel{this};
    m_install_note->setWordWrap(true);
    m_auto = new QCheckBox{tr("Check for B3 Hive updates automatically"), this};

    auto* buttons{new QHBoxLayout};
    buttons->addWidget(m_check);
    buttons->addWidget(m_download);
    buttons->addWidget(m_install);
    buttons->addStretch();

    auto* v{new QVBoxLayout{box}};
    v->addLayout(form);
    v->addLayout(buttons);
    v->addWidget(m_install_note);
    v->addWidget(m_auto);

    auto* outer{new QVBoxLayout{this}};
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(box);

    connect(m_check, &QPushButton::clicked, m_controller, &UpdateController::checkNow);
    connect(m_download, &QPushButton::clicked, m_controller, &UpdateController::startDownload);
    connect(m_install, &QPushButton::clicked, this, &UpdatePanel::onInstallClicked);
    connect(m_auto, &QCheckBox::toggled, m_controller, &UpdateController::setAutoCheckEnabled);
    connect(m_controller, &UpdateController::stateChanged, this, &UpdatePanel::refresh);
    refresh();
}

void UpdatePanel::onInstallClicked()
{
    if (!m_controller->installSupported()) return;
    // Explicit warning before anything is interrupted: installing stops
    // synchronization, staking, validation and wallet activity.
    const auto answer{QMessageBox::question(
        this, tr("Install B3 Hive update"),
        tr("Installing will shut down B3 Hive in an orderly way, stopping "
           "synchronization, staking, validation and wallet activity, and then "
           "launch the verified installer.\n\nInstall the B3 Hive update now?"),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)};
    if (answer == QMessageBox::Yes) m_controller->requestInstallAndRestart();
}

void UpdatePanel::refresh()
{
    m_installed->setText(m_controller->installedVersion());
    const QSignalBlocker auto_blocker{m_auto};
    m_auto->setChecked(m_controller->autoCheckEnabled());
    m_auto->setEnabled(m_controller->configured());
    const bool install_supported{m_controller->installSupported()};
    m_install->setText(install_supported ? tr("Install and restart")
                                         : tr("Automatic install unavailable"));
    if (!m_controller->configured()) {
        m_install_note->setText(
            tr("Update checking and downloads are disabled because this build has no pinned "
               "release channel."));
    } else if (install_supported) {
        m_install_note->setText(
            tr("Installation closes B3 Hive cleanly, replaces the application, and restarts it."));
    } else {
        m_install_note->setText(
            tr("This build can verify and download an update, but it cannot replace or restart "
               "B3 Hive automatically. No automatic-install claim is made."));
    }

    if (!m_controller->configured()) {
        m_status->setText(tr("Updates are not configured in this build."));
        m_check->setEnabled(false);
        m_download->setEnabled(false);
        m_install->setEnabled(false);
        return;
    }
    const UpdateState s{m_controller->state()};
    const bool busy{m_controller->busy()};
    m_latest->setText(m_controller->latestVersion().isEmpty() ? tr("—") : m_controller->latestVersion());
    m_notes->setText(m_controller->notesDigest().isEmpty() ? tr("—") : m_controller->notesDigest());
    m_size->setText(m_controller->downloadSize() == 0 ? tr("—")
                                                      : tr("%1 MB").arg(QString::number(
                                                            m_controller->downloadSize() / 1e6, 'f', 1)));
    m_check->setEnabled(!busy && (s == UpdateState::IDLE || s == UpdateState::UPDATE_AVAILABLE ||
                                  s == UpdateState::FAILED || s == UpdateState::READY_TO_INSTALL));
    m_download->setEnabled(!busy && s == UpdateState::UPDATE_AVAILABLE);
    m_install->setEnabled(install_supported && !busy && s == UpdateState::READY_TO_INSTALL);

    switch (s) {
    case UpdateState::IDLE: m_status->setText(tr("B3 Hive is up to date.")); break;
    case UpdateState::CHECKING: m_status->setText(tr("Checking…")); break;
    case UpdateState::UPDATE_AVAILABLE: m_status->setText(tr("A B3 Hive update is available.")); break;
    case UpdateState::DOWNLOADING: m_status->setText(tr("Downloading…")); break;
    case UpdateState::READY_TO_INSTALL:
        m_status->setText(install_supported
                              ? tr("Update downloaded and verified; ready to install.")
                              : tr("Update downloaded and verified. Automatic installation is unavailable."));
        break;
    case UpdateState::AWAITING_SHUTDOWN: m_status->setText(tr("Waiting for orderly shutdown…")); break;
    case UpdateState::INSTALLING: m_status->setText(tr("Handing off to the installer…")); break;
    case UpdateState::FAILED:
        m_status->setText(tr("Update step failed (%1). The installed B3 Hive is unchanged.")
                              .arg(m_controller->lastError()));
        break;
    case UpdateState::UNCONFIGURED: m_status->setText(tr("Updates are not configured.")); break;
    }
}
