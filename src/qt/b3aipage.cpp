// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <qt/b3aipage.h>

#include <qt/b3aiclient.h>
#include <qt/b3ailessonstore.h>
#include <qt/b3aipublicdata.h>
#include <qt/b3theme.h>

#include <interfaces/node.h>
#include <univalue.h>

#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTextDocument>
#include <QUrl>
#include <QVBoxLayout>

#include <stdexcept>

namespace {
constexpr int MAX_DISPLAY_CHARS{20000};

QLabel* Note(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    B3Theme::markTextRole(label, QStringLiteral("secondary"));
    return label;
}

QString Bounded(const QString& text)
{
    if (text.size() <= MAX_DISPLAY_CHARS) return text;
    return text.left(MAX_DISPLAY_CHARS) + QStringLiteral("\n[Display truncated]");
}

bool AllowedNodeRead(const QString& name)
{
    return name == QStringLiteral("getblockchaininfo") ||
           name == QStringLiteral("getnetworkinfo") ||
           name == QStringLiteral("getfinalitystatus") ||
           name == QStringLiteral("listflowmeshmarkets");
}
} // namespace

B3AiPage::B3AiPage(interfaces::Node* node, const QString& lessonPath, QWidget* parent)
    : QWidget{parent}, m_node{node}, m_client{new B3AiClient(this)}
{
    setObjectName(QStringLiteral("B3AiPage"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(B3Theme::kSpaceLg, B3Theme::kSpaceLg,
                               B3Theme::kSpaceLg, B3Theme::kSpaceLg);
    layout->setSpacing(B3Theme::kSpaceMd);

    auto* hero = new QFrame(this);
    hero->setProperty("b3surface", QStringLiteral("hero"));
    auto* heroLayout = new QVBoxLayout(hero);
    heroLayout->setContentsMargins(20, 16, 20, 16);
    auto* eyebrow = new QLabel(tr("B3 HIVE  /  STUDENT AI"), hero);
    B3Theme::markTextRole(eyebrow, QStringLiteral("eyebrow"));
    heroLayout->addWidget(eyebrow);
    auto* heading = new QLabel(tr("Learn B3. Get things done."), hero);
    heading->setWordWrap(true);
    B3Theme::markTextRole(heading, QStringLiteral("h1"));
    heroLayout->addWidget(heading);
    heroLayout->addWidget(Note(tr("Ask a question, explore B3, or work through a task together."), hero));
    m_status = Note(tr("Not connected · node tools off"), hero);
    m_status->setObjectName(QStringLiteral("aiStatus"));
    B3Theme::markTextRole(m_status, QStringLiteral("status"));
    heroLayout->addWidget(m_status);
    layout->addWidget(hero);

    m_error = Note({}, this);
    m_error->setObjectName(QStringLiteral("aiError"));
    B3Theme::markTextRole(m_error, QStringLiteral("negative"));
    m_error->hide();
    layout->addWidget(m_error);
    m_tabs = new QTabWidget(this);
    m_tabs->setObjectName(QStringLiteral("aiTabs"));
    layout->addWidget(m_tabs, 1);

    auto* chat = new QWidget(m_tabs);
    auto* chatLayout = new QVBoxLayout(chat);
    chatLayout->setContentsMargins(16, 16, 16, 16);
    auto* starters = new QHBoxLayout();
    m_starterLayout = starters;
    const QList<QPair<QString, QString>> prompts{
        {tr("Explain B3"), tr("Explain B3, staking, assets and FlowMesh using the reviewed lessons. Cite your sources and distinguish implemented features from proposals.")},
        {tr("Check node health"), tr("Check my B3 node health using the available read tools. Explain synchronization, network and finality status, and distinguish observations from conclusions.")},
        {tr("Show FlowMesh markets"), tr("Show the FlowMesh markets returned by this node. Explain any reported status without treating it as proof of readiness or settlement.")},
    };
    for (int i = 0; i < prompts.size(); ++i) {
        auto* button = new QPushButton(prompts[i].first, chat);
        button->setObjectName(QStringLiteral("aiStarter%1").arg(i));
        connect(button, &QPushButton::clicked, this, [this, text = prompts[i].second] {
            m_composer->setPlainText(text);
            m_composer->setFocus();
        });
        starters->addWidget(button);
    }
    chatLayout->addLayout(starters);
    m_transcript = new QTextBrowser(chat);
    m_transcript->setObjectName(QStringLiteral("aiTranscript"));
    m_transcript->setOpenLinks(false);
    m_transcript->setOpenExternalLinks(false);
    m_transcript->document()->setMaximumBlockCount(700);
    chatLayout->addWidget(m_transcript, 1);
    m_evidence = new QPlainTextEdit(chat);
    m_evidence->setObjectName(QStringLiteral("aiEvidence"));
    m_evidence->setReadOnly(true);
    m_evidence->setPlaceholderText(tr("Reviewed lesson sources and actual node tool results appear here."));
    m_evidence->document()->setMaximumBlockCount(500);
    m_evidence->setMaximumHeight(120);
    m_evidence->hide();
    chatLayout->addWidget(m_evidence);
    m_composer = new QPlainTextEdit(chat);
    m_composer->setObjectName(QStringLiteral("aiComposer"));
    m_composer->setPlaceholderText(tr("Ask about B3 or describe a task…"));
    m_composer->setMaximumHeight(90);
    chatLayout->addWidget(m_composer);
    auto* chatActions = new QHBoxLayout();
    m_chatActions = chatActions;
    m_newConversation = new QPushButton(tr("New conversation"), chat);
    m_newConversation->setObjectName(QStringLiteral("aiNewConversation"));
    m_teach = new QPushButton(tr("Teach from this answer"), chat);
    m_teach->setObjectName(QStringLiteral("aiTeachAnswer"));
    m_retry = new QPushButton(tr("Retry last request"), chat);
    m_retry->setObjectName(QStringLiteral("aiRetry"));
    m_retry->hide();
    m_send = new QPushButton(tr("Send"), chat);
    m_send->setObjectName(QStringLiteral("aiSend"));
    m_send->setProperty("b3variant", QStringLiteral("primary"));
    chatActions->addWidget(m_newConversation);
    chatActions->addWidget(m_teach);
    chatActions->addWidget(m_retry);
    chatActions->addStretch();
    chatActions->addWidget(m_send);
    chatLayout->addLayout(chatActions);
    chatLayout->addWidget(Note(tr("Ctrl+Enter to send. Review answers and their evidence."), chat));
    m_tabs->addTab(chat, tr("Chat"));

    auto* lessons = new QWidget(m_tabs);
    auto* lessonLayout = new QVBoxLayout(lessons);
    lessonLayout->setContentsMargins(16, 16, 16, 16);
    lessonLayout->addWidget(Note(tr("Reviewed lessons provide reference context. Drafts are excluded until approved. Saving lessons does not train model weights."), lessons));
    m_lessonList = new QListWidget(lessons);
    m_lessonList->setObjectName(QStringLiteral("aiLessonList"));
    m_lessonList->setMaximumHeight(150);
    lessonLayout->addWidget(m_lessonList);
    auto* lessonForm = new QFormLayout();
    lessonForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_question = new QLineEdit(lessons);
    m_question->setObjectName(QStringLiteral("aiLessonQuestion"));
    m_answer = new QPlainTextEdit(lessons);
    m_answer->setObjectName(QStringLiteral("aiLessonAnswer"));
    m_answer->setPlaceholderText(tr("Write a verified explanation or correct the copied model answer."));
    m_source = new QLineEdit(lessons);
    m_source->setObjectName(QStringLiteral("aiLessonSource"));
    m_source->setPlaceholderText(tr("Source file, revision, or other evidence"));
    lessonForm->addRow(tr("Question"), m_question);
    lessonForm->addRow(tr("Corrected answer"), m_answer);
    lessonForm->addRow(tr("Source"), m_source);
    lessonLayout->addLayout(lessonForm, 1);
    m_lessonNotice = Note({}, lessons);
    m_lessonNotice->setObjectName(QStringLiteral("aiLessonNotice"));
    lessonLayout->addWidget(m_lessonNotice);
    auto* lessonActions = new QHBoxLayout();
    m_lessonActions = lessonActions;
    m_saveDraft = new QPushButton(tr("Save draft"), lessons);
    m_saveDraft->setObjectName(QStringLiteral("aiSaveDraft"));
    m_approve = new QPushButton(tr("Approve lesson"), lessons);
    m_approve->setObjectName(QStringLiteral("aiApproveLesson"));
    m_approve->setProperty("b3variant", QStringLiteral("primary"));
    m_remove = new QPushButton(tr("Remove custom"), lessons);
    m_remove->setObjectName(QStringLiteral("aiRemoveLesson"));
    lessonActions->addWidget(m_saveDraft);
    lessonActions->addWidget(m_approve);
    lessonActions->addWidget(m_remove);
    lessonActions->addStretch();
    lessonLayout->addLayout(lessonActions);
    m_tabs->addTab(lessons, tr("Lessons"));

    auto* connection = new QScrollArea(m_tabs);
    connection->setWidgetResizable(true);
    connection->setFrameShape(QFrame::NoFrame);
    auto* connectionContent = new QWidget(connection);
    auto* connectionLayout = new QVBoxLayout(connectionContent);
    connectionLayout->setContentsMargins(20, 20, 20, 20);
    connectionLayout->setSpacing(16);
    connectionLayout->addWidget(Note(tr("Connect your own model service using the chat-completions API. Applying settings validates the configuration; a connection is attempted only when you send a message."), connectionContent));
    QSettings settings;
    auto* connectionForm = new QFormLayout();
    connectionForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_endpoint = new QLineEdit(settings.value(QStringLiteral("B3Assistant/endpoint"), QStringLiteral("http://127.0.0.1:8080/v1/chat/completions")).toString(), connectionContent);
    m_endpoint->setObjectName(QStringLiteral("aiEndpoint"));
    m_model = new QLineEdit(settings.value(QStringLiteral("B3Assistant/model")).toString(), connectionContent);
    m_model->setObjectName(QStringLiteral("aiModelName"));
    m_model->setPlaceholderText(tr("Model name served by your endpoint"));
    m_key = new QLineEdit(connectionContent);
    m_key->setObjectName(QStringLiteral("aiApiKey"));
    m_key->setEchoMode(QLineEdit::Password);
    m_key->setPlaceholderText(tr("Optional · kept in memory for this session"));
    connectionForm->addRow(tr("Endpoint"), m_endpoint);
    connectionForm->addRow(tr("Model"), m_model);
    connectionForm->addRow(tr("API key"), m_key);
    connectionLayout->addLayout(connectionForm);
    connectionLayout->addWidget(Note(tr("Chat text and reviewed lesson context are sent to this endpoint. The endpoint and model name are saved locally; the API key is never saved."), connectionContent));
    m_nodeTools = new QCheckBox(tr("Allow public B3 node reads"), connectionContent);
    m_nodeTools->setObjectName(QStringLiteral("aiNodeTools"));
    m_nodeTools->setChecked(false);
    m_nodeTools->setEnabled(m_node != nullptr);
    connectionLayout->addWidget(m_nodeTools);
    connectionLayout->addWidget(Note(tr("When enabled, public blockchain, network, finality and market status is shared with the configured model. Wallet accounts, balances, endpoints and local signature details are excluded. Tools are off by default; spending, signing and shell access are unavailable."), connectionContent));
    if (!m_node) connectionLayout->addWidget(Note(tr("Node reads are unavailable in this preview."), connectionContent));
    m_apply = new QPushButton(tr("Apply connection"), connectionContent);
    m_apply->setObjectName(QStringLiteral("aiApplyConnection"));
    m_apply->setProperty("b3variant", QStringLiteral("primary"));
    connectionLayout->addWidget(m_apply, 0, Qt::AlignLeft);
    connectionLayout->addWidget(Note(tr("Changing the connection or tool access starts a fresh model conversation. No model is bundled, downloaded or started by this page."), connectionContent));
    connectionLayout->addStretch();
    connection->setWidget(connectionContent);
    m_tabs->addTab(connection, tr("Connection"));

    const QString path{lessonPath.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/student-ai/lessons.json")
        : lessonPath};
    m_lessons = std::make_unique<B3AiLessonStore>(path);
    for (const auto& seed : m_lessons->lessons()) m_seedIds.insert(seed.id);
    QString lessonError;
    const bool directoryReady{QDir().mkpath(QFileInfo(path).absolutePath())};
    const bool loaded{m_lessons->load(&lessonError)};
    m_lessonWritable = directoryReady && loaded;
    if (!m_lessonWritable) {
        m_lessonNotice->setText(tr("Lesson storage is unavailable; saving is disabled. %1").arg(loaded ? tr("Could not create the lesson directory.") : lessonError));
    }
    refreshLessons();

    // The client checks consent and executes on its worker. Capture no page
    // or widgets: a canceled request may outlive this page's model session.
    m_client->setToolExecutor([node = m_node](const QString& name, const QJsonObject& args) -> QJsonValue {
        if (!AllowedNodeRead(name) || !args.isEmpty()) {
            throw std::runtime_error("This node operation is not permitted.");
        }
        if (!node) throw std::runtime_error("No B3 node is attached.");
        try {
            const UniValue response{node->executeRpc(name.toStdString(), UniValue{UniValue::VARR}, "")};
            QJsonParseError parseError;
            // An array wrapper accepts object, array and scalar UniValue results.
            const auto parsed{QJsonDocument::fromJson("[" + QByteArray::fromStdString(response.write()) + "]", &parseError)};
            if (parseError.error != QJsonParseError::NoError || !parsed.isArray() || parsed.array().size() != 1) {
                throw std::runtime_error("The node returned invalid JSON.");
            }
            return B3AiPublicData::SanitizeResult(name, parsed.array().at(0));
        } catch (const UniValue&) {
            return QJsonObject{{QStringLiteral("error"), tr("The B3 node could not provide this public status read.")}};
        } catch (const std::exception&) {
            return QJsonObject{{QStringLiteral("error"), tr("The B3 node could not provide this public status read.")}};
        }
    });
    connect(m_client, &B3AiClient::assistantMessage, this, [this](const QString& text) {
        m_lastAnswer = text;
        m_requestFailed = false;
        appendMessage(tr("Student"), text);
        updateControls();
    });
    connect(m_client, &B3AiClient::toolResult, this, [this](const QString& name, const QString& json) {
        appendEvidence(tr("Node read: %1").arg(name), json);
    });
    connect(m_client, &B3AiClient::errorOccurred, this, [this](const QString& error) {
        m_requestFailed = !m_lastPrompt.isEmpty();
        showError(error);
        appendMessage(tr("Request failed"), error);
        updateControls();
    });
    connect(m_client, &B3AiClient::busyChanged, this, [this](bool) { updateControls(); });
    connect(m_apply, &QPushButton::clicked, this, &B3AiPage::configureConnection);
    const auto connectionEdited = [this] { m_configured = false; updateControls(); };
    for (auto* field : {m_endpoint, m_model, m_key}) connect(field, &QLineEdit::textChanged, this, connectionEdited);
    connect(m_nodeTools, &QCheckBox::toggled, this, connectionEdited);
    connect(m_composer, &QPlainTextEdit::textChanged, this, &B3AiPage::updateControls);
    connect(m_send, &QPushButton::clicked, this, [this] { sendUserMessage(); });
    connect(m_retry, &QPushButton::clicked, this, [this] { sendUserMessage(true); });
    auto* sendShortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), m_composer);
    connect(sendShortcut, &QShortcut::activated, this, [this] { if (m_send->isEnabled()) sendUserMessage(); });
    connect(m_newConversation, &QPushButton::clicked, this, &B3AiPage::newConversation);
    connect(m_lessonList, &QListWidget::currentItemChanged, this, [this] { selectLesson(); });
    connect(m_saveDraft, &QPushButton::clicked, this, &B3AiPage::saveDraft);
    connect(m_approve, &QPushButton::clicked, this, &B3AiPage::approveLesson);
    connect(m_remove, &QPushButton::clicked, this, &B3AiPage::removeLesson);
    const auto lessonEdited = [this] {
        if (!m_loadingLesson) {
            m_lessonDirty = true;
            if (m_lessonWritable && m_lessonList->currentItem()) {
                m_lessonNotice->setText(tr("Edits are unsaved. Save as a new draft before approval."));
            }
        }
        updateControls();
    };
    connect(m_question, &QLineEdit::textChanged, this, lessonEdited);
    connect(m_answer, &QPlainTextEdit::textChanged, this, lessonEdited);
    connect(m_source, &QLineEdit::textChanged, this, lessonEdited);
    connect(m_teach, &QPushButton::clicked, this, [this] {
        m_lessonList->setCurrentRow(-1);
        m_question->setText(m_lastPrompt);
        m_answer->setPlainText(m_lastAnswer);
        m_source->clear();
        if (m_lessonWritable) m_lessonNotice->setText(tr("Unsaved draft copied from model output. Verify the answer and add a source before saving."));
        m_tabs->setCurrentIndex(1);
    });
    newConversation();
}

B3AiPage::~B3AiPage() = default;

void B3AiPage::detachNode()
{
    // This is called while the Node still exists. Cancellation invalidates
    // model callbacks; drainage finishes any read already using the Node.
    m_client->stopAndDrain();
    m_client->setToolExecutor({});
    m_node = nullptr;
    m_configured = false;
    m_nodeTools->setChecked(false);
    m_nodeTools->setEnabled(false);
    newConversation();
}

void B3AiPage::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    const auto direction{event->size().width() < 680 ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight};
    m_starterLayout->setDirection(direction);
    m_chatActions->setDirection(direction);
    m_lessonActions->setDirection(direction);
}

void B3AiPage::appendMessage(const QString& role, const QString& text)
{
    m_transcript->append(QStringLiteral("<p><b style=\"color:%1\">%2</b><br>%3</p>")
        .arg(B3Theme::kAccent.name(), role.toHtmlEscaped(), Bounded(text).toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>"))));
}

void B3AiPage::appendEvidence(const QString& title, const QString& text)
{
    m_evidence->appendPlainText(title + QLatin1Char('\n') + Bounded(text) + QLatin1Char('\n'));
    m_evidence->show();
}

void B3AiPage::showError(const QString& error)
{
    m_error->setText(Bounded(error));
    m_error->setVisible(!error.isEmpty());
}

void B3AiPage::updateControls()
{
    const bool busy{m_client->busy()};
    m_status->setText(busy ? tr("Waiting for the configured model…") : m_configured
        ? tr("Ready to send · %1 · node tools %2").arg(m_model->text(), m_nodeTools->isChecked() ? tr("on") : tr("off"))
        : tr("Not connected · apply a model connection to begin"));
    m_send->setEnabled(m_configured && !busy && !m_composer->toPlainText().trimmed().isEmpty());
    m_composer->setEnabled(!busy);
    m_newConversation->setEnabled(true);
    m_newConversation->setText(busy ? tr("Cancel request") : tr("New conversation"));
    m_retry->setVisible(m_requestFailed);
    m_retry->setEnabled(m_configured && !busy);
    m_teach->setEnabled(!busy && !m_lastAnswer.isEmpty());
    m_apply->setEnabled(!busy);
    for (auto* field : {m_endpoint, m_model, m_key}) field->setEnabled(!busy);
    m_nodeTools->setEnabled(!busy && m_node);
    m_saveDraft->setEnabled(m_lessonWritable && !m_question->text().trimmed().isEmpty() &&
                            !m_answer->toPlainText().trimmed().isEmpty() && !m_source->text().trimmed().isEmpty());
    auto* item = m_lessonList->currentItem();
    const bool custom{item && !item->data(Qt::UserRole + 1).toBool()};
    const bool approved{item && item->data(Qt::UserRole + 2).toBool()};
    m_approve->setEnabled(m_lessonWritable && custom && !approved && !m_lessonDirty);
    m_remove->setEnabled(m_lessonWritable && custom);
}

void B3AiPage::configureConnection()
{
    if (m_client->busy()) return;
    QString error;
    if (!m_client->configure(QUrl(m_endpoint->text().trimmed()), m_model->text().trimmed(),
                             m_key->text(), m_nodeTools->isChecked(), &error)) {
        showError(error);
        return;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("B3Assistant/endpoint"), m_endpoint->text().trimmed());
    settings.setValue(QStringLiteral("B3Assistant/model"), m_model->text().trimmed());
    // Neither the key nor tool consent is persisted. Reconfiguration clears
    // the model history so earlier tool results cannot move to another service.
    m_configured = true;
    newConversation();
    appendMessage(tr("Connection"), tr("Configuration applied. Send a message to contact the model."));
    m_tabs->setCurrentIndex(0);
    updateControls();
}

void B3AiPage::sendUserMessage(bool retry)
{
    if (!m_configured || m_client->busy()) return;
    const QString prompt{retry ? m_lastPrompt : m_composer->toPlainText().trimmed()};
    if (prompt.isEmpty()) return;
    const QString context{retry ? m_lastContext : m_lessons->contextFor(prompt, 8192)};
    QString error;
    if (!m_client->sendMessage(prompt, context, &error)) {
        showError(error);
        return;
    }
    if (!retry) appendMessage(tr("You"), prompt);
    m_lastPrompt = prompt;
    m_lastContext = context;
    m_lastAnswer.clear();
    m_requestFailed = false;
    showError({});
    m_composer->clear();
    if (!context.isEmpty()) appendEvidence(tr("Reviewed lesson context sent with this request"), context);
    updateControls();
}

void B3AiPage::newConversation()
{
    m_client->clearConversation();
    m_transcript->clear();
    m_evidence->clear();
    m_evidence->hide();
    m_lastPrompt.clear();
    m_lastAnswer.clear();
    m_lastContext.clear();
    m_requestFailed = false;
    showError({});
    appendMessage(tr("Welcome"), tr("Hello. Ask me a question about B3 or describe a task. "
        "To start chatting, choose your model in Connection. You can help me learn through Lessons."));
    updateControls();
}

void B3AiPage::refreshLessons()
{
    const QString selected{m_lessonList->currentItem() ? m_lessonList->currentItem()->data(Qt::UserRole).toString() : QString{}};
    m_lessonList->clear();
    for (const auto& lesson : m_lessons->lessons()) {
        const bool seed{m_seedIds.contains(lesson.id)};
        auto* item = new QListWidgetItem(QStringLiteral("%1  ·  %2").arg(
            seed ? tr("Reviewed seed") : lesson.approved ? tr("Reviewed") : tr("Draft"), lesson.question), m_lessonList);
        item->setData(Qt::UserRole, lesson.id);
        item->setData(Qt::UserRole + 1, seed);
        item->setData(Qt::UserRole + 2, lesson.approved);
        item->setToolTip(lesson.source.toHtmlEscaped());
        if (lesson.id == selected) m_lessonList->setCurrentItem(item);
    }
}

void B3AiPage::selectLesson()
{
    auto* item = m_lessonList->currentItem();
    m_loadingLesson = true;
    if (item) {
        for (const auto& lesson : m_lessons->lessons()) {
            if (lesson.id != item->data(Qt::UserRole).toString()) continue;
            m_question->setText(lesson.question);
            m_answer->setPlainText(lesson.answer);
            m_source->setText(lesson.source);
            if (m_lessonWritable) m_lessonNotice->setText(lesson.approved
                ? tr("Reviewed reference. Edit and save as a new draft to propose a correction.")
                : tr("Draft excluded from chat context. Check its answer and source before approving."));
            break;
        }
    }
    m_loadingLesson = false;
    m_lessonDirty = false;
    updateControls();
}

void B3AiPage::saveDraft()
{
    if (!m_saveDraft->isEnabled()) return;
    const QString question{m_question->text().trimmed()};
    const QString answer{m_answer->toPlainText().trimmed()};
    const QString source{m_source->text().trimmed()};
    QString error;
    if (!m_lessons->addDraft(question, answer, source, &error)) {
        m_lessonNotice->setText(error);
        return;
    }
    refreshLessons();
    for (const auto& lesson : m_lessons->lessons()) {
        if (lesson.question != question || lesson.answer != answer || lesson.source != source) continue;
        for (int row = 0; row < m_lessonList->count(); ++row) {
            if (m_lessonList->item(row)->data(Qt::UserRole).toString() == lesson.id) {
                m_lessonList->setCurrentRow(row);
                break;
            }
        }
        break;
    }
    m_lessonNotice->setText(tr("Draft saved. Review and approve it when verified; it is not yet used in chat."));
    updateControls();
}

void B3AiPage::approveLesson()
{
    if (!m_approve->isEnabled()) return;
    QString error;
    const QString id{m_lessonList->currentItem()->data(Qt::UserRole).toString()};
    if (!m_lessons->approve(id, &error)) { m_lessonNotice->setText(error); return; }
    refreshLessons();
    m_lessonNotice->setText(tr("Lesson approved. It can now be retrieved as reference context; model weights are unchanged."));
    updateControls();
}

void B3AiPage::removeLesson()
{
    if (!m_remove->isEnabled()) return;
    QString error;
    const QString id{m_lessonList->currentItem()->data(Qt::UserRole).toString()};
    if (!m_lessons->remove(id, &error)) { m_lessonNotice->setText(error); return; }
    refreshLessons();
    if (!m_lessonList->currentItem()) {
        m_question->clear();
        m_answer->clear();
        m_source->clear();
    }
    m_lessonNotice->setText(tr("Custom lesson removed."));
    updateControls();
}
