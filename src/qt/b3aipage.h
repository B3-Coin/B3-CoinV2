// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_QT_B3AIPAGE_H
#define BITCOIN_QT_B3AIPAGE_H

#include <QWidget>
#include <QSet>

#include <memory>

namespace interfaces { class Node; }
class B3AiClient;
class B3AiLessonStore;
QT_BEGIN_NAMESPACE
class QCheckBox;
class QBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QResizeEvent;
class QTabWidget;
class QTextBrowser;
QT_END_NAMESPACE

//! A model client and reviewed lesson editor; never a signer or consensus participant.
class B3AiPage : public QWidget
{
    Q_OBJECT

public:
    explicit B3AiPage(interfaces::Node* node = nullptr, const QString& lessonPath = {}, QWidget* parent = nullptr);
    ~B3AiPage() override;
    //! Drain pending node reads before the application tears down its core.
    void detachNode();
    //! Inspect a bounded local progress report; never starts training or a node.
    bool loadTrainingReport(const QString& path);

private:
    void resizeEvent(QResizeEvent* event) override;
    void appendMessage(const QString& role, const QString& text);
    void appendEvidence(const QString& title, const QString& text);
    void showError(const QString& error);
    void updateControls();
    void configureConnection();
    void sendUserMessage(bool retry = false);
    void newConversation();
    void refreshLessons();
    void selectLesson();
    void saveDraft();
    void approveLesson();
    void removeLesson();
    void refreshTrainingReport();
    void clearTrainingValues();

    interfaces::Node* m_node{nullptr};
    B3AiClient* m_client{nullptr};
    std::unique_ptr<B3AiLessonStore> m_lessons;
    QTabWidget* m_tabs{nullptr};
    QLabel* m_status{nullptr};
    QLabel* m_error{nullptr};
    QTextBrowser* m_transcript{nullptr};
    QPlainTextEdit* m_evidence{nullptr};
    QPlainTextEdit* m_composer{nullptr};
    QPushButton* m_send{nullptr};
    QPushButton* m_newConversation{nullptr};
    QPushButton* m_retry{nullptr};
    QPushButton* m_teach{nullptr};
    QLineEdit* m_endpoint{nullptr};
    QLineEdit* m_model{nullptr};
    QLineEdit* m_key{nullptr};
    QCheckBox* m_nodeTools{nullptr};
    QPushButton* m_apply{nullptr};
    QListWidget* m_lessonList{nullptr};
    QLineEdit* m_question{nullptr};
    QPlainTextEdit* m_answer{nullptr};
    QLineEdit* m_source{nullptr};
    QLabel* m_lessonNotice{nullptr};
    QPushButton* m_saveDraft{nullptr};
    QPushButton* m_approve{nullptr};
    QPushButton* m_remove{nullptr};
    QBoxLayout* m_starterLayout{nullptr};
    QBoxLayout* m_chatActions{nullptr};
    QBoxLayout* m_lessonActions{nullptr};
    QLineEdit* m_trainingPath{nullptr};
    QLabel* m_trainingNotice{nullptr};
    QLabel* m_trainingStatus{nullptr};
    QLabel* m_trainingParameters{nullptr};
    QLabel* m_trainingSavedUpdates{nullptr};
    QLabel* m_trainingAttemptedUpdates{nullptr};
    QLabel* m_trainingTokens{nullptr};
    QLabel* m_trainingCurrentLoss{nullptr};
    QLabel* m_trainingBestLoss{nullptr};
    QLabel* m_trainingElapsed{nullptr};
    QPushButton* m_trainingRefresh{nullptr};
    QString m_trainingReportPath;
    bool m_configured{false};
    bool m_lessonWritable{false};
    bool m_loadingLesson{false};
    bool m_lessonDirty{false};
    bool m_requestFailed{false};
    QString m_lastPrompt;
    QString m_lastAnswer;
    QString m_lastContext;
    QSet<QString> m_seedIds;
};

#endif // BITCOIN_QT_B3AIPAGE_H
