// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/Debugger/BranchWatchDialog.h"

#include <algorithm>
#include <optional>
#include <utility>

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QShortcut>
#include <QSortFilterProxyModel>
#include <QStatusBar>
#include <QString>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QVBoxLayout>
#include <QVariant>
#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/CommonFuncs.h"
#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/Debugger/BranchWatch.h"
#include "Core/Debugger/PPCDebugInterface.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "DolphinQt/Debugger/BranchWatchTableModel.h"
#include "DolphinQt/Debugger/CodeWidget.h"
#include "DolphinQt/QtUtils/DolphinFileDialog.h"
#include "DolphinQt/QtUtils/ModalMessageBox.h"
#include "DolphinQt/QtUtils/SetWindowDecorations.h"
#include "DolphinQt/Settings.h"

class BranchWatchProxyModel final : public QSortFilterProxyModel
{
  friend BranchWatchDialog;

public:
  explicit BranchWatchProxyModel(const Core::BranchWatch& branch_watch, QObject* parent = nullptr)
      : QSortFilterProxyModel(parent), m_branch_watch(branch_watch)
  {
  }

  BranchWatchTableModel* sourceModel() const
  {
    return static_cast<BranchWatchTableModel*>(QSortFilterProxyModel::sourceModel());
  }
  void setSourceModel(BranchWatchTableModel* source_model)
  {
    QSortFilterProxyModel::setSourceModel(source_model);
  }

  // Virtual setSourceModel is forbidden for type-safety reasons. See sourceModel().
  [[noreturn]] void setSourceModel(QAbstractItemModel* source_model) override { Crash(); }
  bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override;

  template <bool BranchWatchProxyModel::*member>
  void OnToggled(bool enabled)
  {
    this->*member = enabled;
    invalidateRowsFilter();
  }
  template <QString BranchWatchProxyModel::*member>
  void OnSymbolTextChanged(const QString& text)
  {
    this->*member = text;
    invalidateRowsFilter();
  }
  template <std::optional<u32> BranchWatchProxyModel::*member>
  void OnAddressTextChanged(const QString& text)
  {
    bool ok = false;
    if (const u32 value = text.toUInt(&ok, 16); ok)
      this->*member = value;
    else
      this->*member = std::nullopt;
    invalidateRowsFilter();
  }
  void OnDelete(QModelIndexList index_list);

  bool IsBranchTypeAllowed(UGeckoInstruction inst) const;
  void SetInspected(const QModelIndex& index);

private:
  const Core::BranchWatch& m_branch_watch;

  QString m_origin_symbol_name = {}, m_destin_symbol_name = {};
  std::optional<u32> m_origin_min, m_origin_max, m_destin_min, m_destin_max;
  bool m_b = {}, m_bl = {}, m_bc = {}, m_bcl = {}, m_blr = {}, m_blrl = {}, m_bclr = {},
       m_bclrl = {}, m_bctr = {}, m_bctrl = {}, m_bcctr = {}, m_bcctrl = {};
  bool m_cond_true = {}, m_cond_false = {};
};

bool BranchWatchProxyModel::filterAcceptsRow(int source_row, const QModelIndex&) const
{
  const Core::BranchWatch::Selection::value_type& value = m_branch_watch.GetSelection()[source_row];
  if (value.condition)
  {
    if (!m_cond_true)
      return false;
  }
  else if (!m_cond_false)
    return false;

  const Core::BranchWatchCollectionKey& k = value.collection_ptr->first;
  if (!IsBranchTypeAllowed(k.original_inst))
    return false;

  if (m_origin_min.has_value() && k.origin_addr < m_origin_min.value())
    return false;
  if (m_origin_max.has_value() && k.origin_addr > m_origin_max.value())
    return false;
  if (m_destin_min.has_value() && k.destin_addr < m_destin_min.value())
    return false;
  if (m_destin_max.has_value() && k.destin_addr > m_destin_max.value())
    return false;

  if (!m_origin_symbol_name.isEmpty())
  {
    if (const QVariant& symbol_name_v = sourceModel()->GetSymbolList()[source_row].origin_name;
        !symbol_name_v.isValid() ||
        !symbol_name_v.value<QString>().contains(m_origin_symbol_name, Qt::CaseInsensitive))
      return false;
  }
  if (!m_destin_symbol_name.isEmpty())
  {
    if (const QVariant& symbol_name_v = sourceModel()->GetSymbolList()[source_row].destin_name;
        !symbol_name_v.isValid() ||
        !symbol_name_v.value<QString>().contains(m_destin_symbol_name, Qt::CaseInsensitive))
      return false;
  }
  return true;
}

void BranchWatchProxyModel::OnDelete(QModelIndexList index_list)
{
  std::transform(index_list.begin(), index_list.end(), index_list.begin(),
                 [this](const QModelIndex& index) { return mapToSource(index); });
  sourceModel()->OnDelete(std::move(index_list));
}

static constexpr bool BranchSavesLR(UGeckoInstruction inst)
{
  DEBUG_ASSERT(inst.OPCD == 18 || inst.OPCD == 16 ||
               (inst.OPCD == 19 && (inst.SUBOP10 == 16 || inst.SUBOP10 == 528)));
  // Every branch instruction uses the same LK field.
  return inst.LK;
}

bool BranchWatchProxyModel::IsBranchTypeAllowed(UGeckoInstruction inst) const
{
  const bool lr_saved = BranchSavesLR(inst);
  switch (inst.OPCD)
  {
  case 18:
    return lr_saved ? m_bl : m_b;
  case 16:
    return lr_saved ? m_bcl : m_bc;
  case 19:
    switch (inst.SUBOP10)
    {
    case 16:
      if ((inst.BO & 0b10100) == 0b10100)  // 1z1zz - Branch always
        return lr_saved ? m_blrl : m_blr;
      return lr_saved ? m_bclrl : m_bclr;
    case 528:
      if ((inst.BO & 0b10100) == 0b10100)  // 1z1zz - Branch always
        return lr_saved ? m_bctrl : m_bctr;
      return lr_saved ? m_bcctrl : m_bcctr;
    }
  }
  return false;
}

void BranchWatchProxyModel::SetInspected(const QModelIndex& index)
{
  sourceModel()->SetInspected(mapToSource(index));
}

BranchWatchDialog::BranchWatchDialog(Core::System& system, Core::BranchWatch& branch_watch,
                                     CodeWidget* code_widget, QWidget* parent)
    : QDialog(parent), m_system(system), m_branch_watch(branch_watch), m_code_widget(code_widget)
{
  setWindowTitle(tr("Branch Watch Tool"));
  setWindowFlags((windowFlags() | Qt::WindowMinMaxButtonsHint) & ~Qt::WindowContextHelpButtonHint);
  SetQWidgetWindowDecorations(this);
  setLayout([this]() {
    auto* layout = new QVBoxLayout;

    // Controls Toolbar (widgets are added later)
    layout->addWidget(m_control_toolbar = new QToolBar);

    // Branch Watch Table
    layout->addWidget(m_table_view = [this]() {
      const auto& ui_settings = Settings::Instance();

      m_table_proxy = new BranchWatchProxyModel(m_branch_watch);
      m_table_proxy->setSourceModel(m_table_model =
                                        new BranchWatchTableModel(m_system, m_branch_watch));
      m_table_proxy->setSortRole(UserRole::SortRole);

      m_table_model->setFont(ui_settings.GetDebugFont());
      connect(&ui_settings, &Settings::DebugFontChanged, m_table_model,
              &BranchWatchTableModel::setFont);

      auto* const table_view = new QTableView;
      table_view->setModel(m_table_proxy);
      table_view->setSortingEnabled(true);
      table_view->sortByColumn(Column::Origin, Qt::AscendingOrder);
      table_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
      table_view->setSelectionBehavior(QAbstractItemView::SelectRows);
      table_view->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
      table_view->setContextMenuPolicy(Qt::CustomContextMenu);
      table_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
      table_view->setCornerButtonEnabled(false);
      table_view->verticalHeader()->hide();

      QHeaderView* const horizontal_header = table_view->horizontalHeader();
      horizontal_header->restoreState(  // Restore column visibility state.
          Settings::GetQSettings()
              .value(QStringLiteral("branchwatchdialog/tableheader/state"))
              .toByteArray());
      horizontal_header->setContextMenuPolicy(Qt::CustomContextMenu);
      horizontal_header->setStretchLastSection(true);
      horizontal_header->setSectionsMovable(true);
      horizontal_header->setFirstSectionMovable(true);

      connect(table_view, &QTableView::clicked, this, &BranchWatchDialog::OnTableClicked);
      connect(table_view, &QTableView::customContextMenuRequested, this,
              &BranchWatchDialog::OnTableContextMenu);
      connect(horizontal_header, &QHeaderView::customContextMenuRequested, this,
              &BranchWatchDialog::OnTableHeaderContextMenu);
      connect(new QShortcut(QKeySequence(Qt::Key_Delete), this), &QShortcut::activated, this,
              &BranchWatchDialog::OnTableDeleteKeypress);

      return table_view;
    }());

    m_mnu_column_visibility = [this]() {
      static constexpr std::array<const char*, Column::NumberOfColumns> headers = {
          QT_TR_NOOP("Instruction"),   QT_TR_NOOP("Condition"),         QT_TR_NOOP("Origin"),
          QT_TR_NOOP("Destination"),   QT_TR_NOOP("Recent Hits"),       QT_TR_NOOP("Total Hits"),
          QT_TR_NOOP("Origin Symbol"), QT_TR_NOOP("Destination Symbol")};

      auto* const menu = new QMenu();
      for (int column = 0; column < Column::NumberOfColumns; ++column)
      {
        QAction* action = menu->addAction(tr(headers[column]), [this, column](bool enabled) {
          m_table_view->setColumnHidden(column, !enabled);
        });
        action->setChecked(!m_table_view->isColumnHidden(column));
        action->setCheckable(true);
      }
      return menu;
    }();

    // Menu Bar
    layout->setMenuBar([this]() {
      QMenuBar* const menu_bar = new QMenuBar;
      menu_bar->setNativeMenuBar(false);

      QMenu* const menu_file = new QMenu(tr("&File"), menu_bar);
      menu_file->addAction(tr("&Save Branch Watch"), this, &BranchWatchDialog::OnSave);
      menu_file->addAction(tr("Save Branch Watch &As..."), this, &BranchWatchDialog::OnSaveAs);
      menu_file->addAction(tr("&Load Branch Watch"), this, &BranchWatchDialog::OnLoad);
      menu_file->addAction(tr("Load Branch Watch &From..."), this, &BranchWatchDialog::OnLoadFrom);
      m_act_autosave = menu_file->addAction(tr("A&uto Save"));
      m_act_autosave->setCheckable(true);
      connect(m_act_autosave, &QAction::toggled, this, &BranchWatchDialog::OnToggleAutoSave);
      menu_bar->addMenu(menu_file);

      QMenu* const menu_tool = new QMenu(tr("&Tool"), menu_bar);
      menu_tool->setToolTipsVisible(true);
      menu_tool->addAction(tr("Hide &Controls"), this, &BranchWatchDialog::OnHideShowControls)
          ->setCheckable(true);
      QAction* const act_ignore_apploader =
          menu_tool->addAction(tr("Ignore &Apploader Branch Hits"));
      act_ignore_apploader->setToolTip(
          tr("This only applies to the initial boot of the emulated software."));
      act_ignore_apploader->setChecked(m_system.IsBranchWatchIgnoreApploader());
      act_ignore_apploader->setCheckable(true);
      connect(act_ignore_apploader, &QAction::toggled, this,
              &BranchWatchDialog::OnToggleIgnoreApploader);

      menu_tool->addMenu(m_mnu_column_visibility)->setText(tr("Column &Visibility"));
      menu_tool->addAction(tr("Wipe &Inspection Data"), this, &BranchWatchDialog::OnWipeInspection);
      menu_tool->addAction(tr("&Help"), this, &BranchWatchDialog::OnHelp);

      menu_bar->addMenu(menu_tool);

      return menu_bar;
    }());

    // Status Bar
    layout->addWidget(m_status_bar = []() {
      auto* const status_bar = new QStatusBar;
      status_bar->setSizeGripEnabled(false);
      return status_bar;
    }());

    // Tool Controls
    m_control_toolbar->addWidget([this]() {
      auto* const layout = new QGridLayout;

      layout->addWidget(m_btn_start_pause = new QPushButton(tr("Start Branch Watch")), 0, 0);
      connect(m_btn_start_pause, &QPushButton::toggled, this, &BranchWatchDialog::OnStartPause);
      m_btn_start_pause->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
      m_btn_start_pause->setCheckable(true);

      layout->addWidget(m_btn_clear_watch = new QPushButton(tr("Clear Branch Watch")), 1, 0);
      connect(m_btn_clear_watch, &QPushButton::pressed, this,
              &BranchWatchDialog::OnClearBranchWatch);
      m_btn_clear_watch->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

      layout->addWidget(m_btn_path_was_taken = new QPushButton(tr("Code Path Was Taken")), 0, 1);
      connect(m_btn_path_was_taken, &QPushButton::pressed, this,
              &BranchWatchDialog::OnCodePathWasTaken);
      m_btn_path_was_taken->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

      layout->addWidget(m_btn_path_not_taken = new QPushButton(tr("Code Path Not Taken")), 1, 1);
      connect(m_btn_path_not_taken, &QPushButton::pressed, this,
              &BranchWatchDialog::OnCodePathNotTaken);
      m_btn_path_not_taken->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

      auto* const group_box = new QGroupBox(tr("Tool Controls"));
      group_box->setLayout(layout);
      group_box->setAlignment(Qt::AlignHCenter);

      return group_box;
    }());

    // Spacer
    m_control_toolbar->addWidget([]() {
      auto* const widget = new QWidget;
      widget->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Preferred);
      return widget;
    }());

    // Branch Type Filter Options
    m_control_toolbar->addWidget([this]() {
      auto* const layout = new QGridLayout;

      const auto routine = [this, layout](const QString& text, const QString& tooltip, int row,
                                          int column, void (BranchWatchProxyModel::*slot)(bool)) {
        QCheckBox* const check_box = new QCheckBox(text);
        check_box->setToolTip(tooltip);
        layout->addWidget(check_box, row, column);
        connect(check_box, &QCheckBox::toggled, [this, slot](bool checked) {
          (m_table_proxy->*slot)(checked);
          UpdateStatus();
        });
        check_box->setChecked(true);
      };

      // clang-format off
      routine(QStringLiteral("b"     ), tr("Branch"                                         ), 0, 0, &BranchWatchProxyModel::OnToggled<&BranchWatchProxyModel::m_b     >);
      routine(QStringLiteral("bl"    ), tr("Branch (LR saved)"                              ), 0, 1, &BranchWatchProxyModel::OnToggled<&BranchWatchProxyModel::m_bl    >);
      routine(QStringLiteral("bc"    ), tr("Branch Conditional"                             ), 0, 2, &BranchWatchProxyModel::OnToggled<&BranchWatchProxyModel::m_bc    >);
      routine(QStringLiteral("bcl"   ), tr("Branch Conditional (LR saved)"                  ), 0, 3, &BranchWatchProxyModel::OnToggled<&BranchWatchProxyModel::m_bcl   >);
      routine(QStringLiteral("blr"   ), tr("Branch to Link Register"                        ), 1, 0, &BranchWatchProxyModel::OnToggled<&BranchWatchProxyModel::m_blr   >);
      routine(QStringLiteral("blrl"  ), tr("Branch to Link Register (LR saved)"             ), 1, 1, &BranchWatchProxyModel::OnToggled<&BranchWatchProxyModel::m_blrl  >);
      routine(QStringLiteral("bclr"  ), tr("Branch Conditional to Link Register"            ), 1, 2, &BranchWatchProxyModel::OnToggled<&BranchWatchProxyModel::m_bclr  >);
      routine(QStringLiteral("bclrl" ), tr("Branch Conditional to Link Register (LR saved)" ), 1, 3, &BranchWatchProxyModel::OnToggled<&BranchWatchProxyModel::m_bclrl >);
      routine(QStringLiteral("bctr"  ), tr("Branch to Count Register"                       ), 2, 0, &BranchWatchProxyModel::OnToggled<&BranchWatchProxyModel::m_bctr  >);
      routine(QStringLiteral("bctrl" ), tr("Branch to Count Register (LR saved)"            ), 2, 1, &BranchWatchProxyModel::OnToggled<&BranchWatchProxyModel::m_bctrl >);
      routine(QStringLiteral("bcctr" ), tr("Branch Conditional to Count Register"           ), 2, 2, &BranchWatchProxyModel::OnToggled<&BranchWatchProxyModel::m_bcctr >);
      routine(QStringLiteral("bcctrl"), tr("Branch Conditional to Count Register (LR saved)"), 2, 3, &BranchWatchProxyModel::OnToggled<&BranchWatchProxyModel::m_bcctrl>);
      // clang-format on

      auto* const group_box = new QGroupBox(tr("Branch Type"));
      group_box->setLayout(layout);
      group_box->setAlignment(Qt::AlignHCenter);

      return group_box;
    }());

    // Origin and Destination Filter Options
    m_control_toolbar->addWidget([this]() {
      auto* const layout = new QGridLayout;

      const auto routine = [this, layout](const QString& text, int row, int column, int width,
                                          void (BranchWatchProxyModel::*slot)(const QString&)) {
        QLineEdit* const line_edit = new QLineEdit;
        layout->addWidget(line_edit, row, column, 1, width);
        connect(line_edit, &QLineEdit::textChanged, [this, slot](const QString& text) {
          (m_table_proxy->*slot)(text);
          UpdateStatus();
        });
        line_edit->setPlaceholderText(text);
        return line_edit;
      };

      // clang-format off
      routine(tr("Origin Symbol"     ), 0, 0, 1, &BranchWatchProxyModel::OnSymbolTextChanged<&BranchWatchProxyModel::m_origin_symbol_name>);
      routine(tr("Origin Min"        ), 1, 0, 1, &BranchWatchProxyModel::OnAddressTextChanged<&BranchWatchProxyModel::m_origin_min>)->setMaxLength(8);
      routine(tr("Origin Max"        ), 2, 0, 1, &BranchWatchProxyModel::OnAddressTextChanged<&BranchWatchProxyModel::m_origin_max>)->setMaxLength(8);
      routine(tr("Destination Symbol"), 0, 1, 1, &BranchWatchProxyModel::OnSymbolTextChanged<&BranchWatchProxyModel::m_destin_symbol_name>);
      routine(tr("Destination Min"   ), 1, 1, 1, &BranchWatchProxyModel::OnAddressTextChanged<&BranchWatchProxyModel::m_destin_min>)->setMaxLength(8);
      routine(tr("Destination Max"   ), 2, 1, 1, &BranchWatchProxyModel::OnAddressTextChanged<&BranchWatchProxyModel::m_destin_max>)->setMaxLength(8);
      // clang-format on

      auto* const group_box = new QGroupBox(tr("Origin and Destination"));
      group_box->setLayout(layout);
      group_box->setAlignment(Qt::AlignHCenter);

      return group_box;
    }());

    // Condition Filter Options
    m_control_toolbar->addWidget([this]() {
      auto* const layout = new QVBoxLayout;
      layout->setAlignment(Qt::AlignHCenter);

      const auto routine = [this, layout](const QString& text,
                                          void (BranchWatchProxyModel::*slot)(bool)) {
        QCheckBox* const check_box = new QCheckBox(text);
        layout->addWidget(check_box);
        connect(check_box, &QCheckBox::toggled, [this, slot](bool checked) {
          (m_table_proxy->*slot)(checked);
          UpdateStatus();
        });
        check_box->setChecked(true);
        return check_box;
      };

      routine(QStringLiteral("true"),
              &BranchWatchProxyModel::OnToggled<&BranchWatchProxyModel::m_cond_true>)
          ->setToolTip(tr("This will also filter unconditional branches.\n"
                          "To filter for or against unconditional branches,\n"
                          "use the Branch Type filter options."));
      routine(QStringLiteral("false"),
              &BranchWatchProxyModel::OnToggled<&BranchWatchProxyModel::m_cond_false>);

      auto* const group_box = new QGroupBox(tr("Condition"));
      group_box->setLayout(layout);
      group_box->setAlignment(Qt::AlignHCenter);

      return group_box;
    }());

    // Misc. Controls
    m_control_toolbar->addWidget([this]() {
      auto* const layout = new QVBoxLayout;

      layout->addWidget(m_btn_was_overwritten = new QPushButton(tr("Branch Was Overwritten")));
      connect(m_btn_was_overwritten, &QPushButton::pressed, this,
              &BranchWatchDialog::OnBranchWasOverwritten);
      m_btn_was_overwritten->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

      layout->addWidget(m_btn_not_overwritten = new QPushButton(tr("Branch Not Overwritten")));
      connect(m_btn_not_overwritten, &QPushButton::pressed, this,
              &BranchWatchDialog::OnBranchNotOverwritten);
      m_btn_not_overwritten->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);

      layout->addWidget(m_btn_wipe_recent_hits = new QPushButton(tr("Wipe Recent Hits")));
      connect(m_btn_wipe_recent_hits, &QPushButton::pressed, this,
              &BranchWatchDialog::OnWipeRecentHits);
      m_btn_wipe_recent_hits->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
      m_btn_wipe_recent_hits->setEnabled(false);

      auto* const group_box = new QGroupBox(tr("Misc. Controls"));
      group_box->setLayout(layout);
      group_box->setAlignment(Qt::AlignHCenter);

      return group_box;
    }());

    connect(m_timer = new QTimer, &QTimer::timeout, this, &BranchWatchDialog::OnTimeout);
    connect(&Settings::Instance(), &Settings::EmulationStateChanged, this,
            &BranchWatchDialog::OnEmulationStateChanged);
    connect(m_table_proxy, &BranchWatchProxyModel::layoutChanged, this,
            &BranchWatchDialog::UpdateStatus);

    return layout;
  }());

  // FIXME: On Linux, Qt6 has recently been resetting column widths to their defaults in many
  // unexpected ways. This affects all kinds of QTables in Dolphin's GUI, so to avoid it in
  // this QTableView, I have deferred this operation. Any earlier, and this would be undone.
  // SetQWidgetWindowDecorations was moved to before these operations for the same reason.
  m_table_view->setColumnWidth(Column::Instruction, 50);
  m_table_view->setColumnWidth(Column::Condition, 50);
  m_table_view->setColumnWidth(Column::OriginSymbol, 250);
  m_table_view->setColumnWidth(Column::DestinSymbol, 250);
  // The default column width (100 units) is fine for the rest.

  const auto& settings = Settings::GetQSettings();
  restoreGeometry(settings.value(QStringLiteral("branchwatchdialog/geometry")).toByteArray());
}

BranchWatchDialog::~BranchWatchDialog()
{
  SaveSettings();
}

static constexpr int BRANCH_WATCH_TOOL_TIMER_DELAY_MS = 100;
static constexpr int BRANCH_WATCH_TOOL_TIMER_PAUSE_ONESHOT_MS = 200;

static bool TimerCondition(const Core::BranchWatch& branch_watch, Core::State state)
{
  return branch_watch.GetRecordingActive() && state > Core::State::Paused;
}

void BranchWatchDialog::hideEvent(QHideEvent* event)
{
  if (m_timer->isActive())
    m_timer->stop();
  QDialog::hideEvent(event);
}

void BranchWatchDialog::showEvent(QShowEvent* event)
{
  if (TimerCondition(m_branch_watch, Core::GetState()))
    m_timer->start(BRANCH_WATCH_TOOL_TIMER_DELAY_MS);
  QDialog::showEvent(event);
}

void BranchWatchDialog::OnStartPause(bool checked)
{
  if (checked)
  {
    m_branch_watch.Start();
    m_btn_start_pause->setText(tr("Pause Branch Watch"));
    // Restart the timer if the situation calls for it, but always turn off single-shot.
    m_timer->setSingleShot(false);
    if (Core::GetState() > Core::State::Paused)
      m_timer->start(BRANCH_WATCH_TOOL_TIMER_DELAY_MS);
  }
  else
  {
    m_branch_watch.Pause();
    m_btn_start_pause->setText(tr("Start Branch Watch"));
    // Schedule one last update in the future in case Branch Watch is in the middle of a hit.
    if (Core::GetState() > Core::State::Paused)
      m_timer->setInterval(BRANCH_WATCH_TOOL_TIMER_PAUSE_ONESHOT_MS);
    m_timer->setSingleShot(true);
  }
  Update();
}

void BranchWatchDialog::OnClearBranchWatch()
{
  {
    const Core::CPUThreadGuard guard{m_system};
    m_table_model->OnClearBranchWatch(guard);
    AutoSave(guard);
  }
  m_btn_wipe_recent_hits->setEnabled(false);
  UpdateStatus();
}

static std::string GetSnapshotDefaultFilepath()
{
  return fmt::format("{}{}.txt", File::GetUserPath(D_DUMPDEBUG_BRANCHWATCH_IDX),
                     SConfig::GetInstance().GetGameID());
}

void BranchWatchDialog::OnSave()
{
  if (!m_branch_watch.CanSave())
  {
    ModalMessageBox::warning(this, tr("Error"), tr("There is nothing to save!"));
    return;
  }

  Save(Core::CPUThreadGuard{m_system}, GetSnapshotDefaultFilepath());
}

void BranchWatchDialog::OnSaveAs()
{
  if (!m_branch_watch.CanSave())
  {
    ModalMessageBox::warning(this, tr("Error"), tr("There is nothing to save!"));
    return;
  }

  const QString filepath = DolphinFileDialog::getSaveFileName(
      this, tr("Save Branch Watch snapshot"),
      QString::fromStdString(File::GetUserPath(D_DUMPDEBUG_BRANCHWATCH_IDX)),
      tr("Text file (*.txt);;All Files (*)"));
  if (filepath.isEmpty())
    return;

  Save(Core::CPUThreadGuard{m_system}, filepath.toStdString());
}

void BranchWatchDialog::OnLoad()
{
  Load(Core::CPUThreadGuard{m_system}, GetSnapshotDefaultFilepath());
}

void BranchWatchDialog::OnLoadFrom()
{
  const QString filepath = DolphinFileDialog::getOpenFileName(
      this, tr("Load Branch Watch snapshot"),
      QString::fromStdString(File::GetUserPath(D_DUMPDEBUG_BRANCHWATCH_IDX)),
      tr("Text file (*.txt);;All Files (*)"), nullptr, QFileDialog::Option::ReadOnly);
  if (filepath.isEmpty())
    return;

  Load(Core::CPUThreadGuard{m_system}, filepath.toStdString());
}

void BranchWatchDialog::OnCodePathWasTaken()
{
  {
    const Core::CPUThreadGuard guard{m_system};
    m_table_model->OnCodePathWasTaken(guard);
    AutoSave(guard);
  }
  m_btn_wipe_recent_hits->setEnabled(true);
  UpdateStatus();
}

void BranchWatchDialog::OnCodePathNotTaken()
{
  {
    const Core::CPUThreadGuard guard{m_system};
    m_table_model->OnCodePathNotTaken(guard);
    AutoSave(guard);
  }
  UpdateStatus();
}

void BranchWatchDialog::OnBranchWasOverwritten()
{
  if (Core::GetState() == Core::State::Uninitialized)
  {
    ModalMessageBox::warning(this, tr("Error"), tr("Core is uninitialized."));
    return;
  }
  {
    const Core::CPUThreadGuard guard{m_system};
    m_table_model->OnBranchWasOverwritten(guard);
    AutoSave(guard);
  }
  UpdateStatus();
}

void BranchWatchDialog::OnBranchNotOverwritten()
{
  if (Core::GetState() == Core::State::Uninitialized)
  {
    ModalMessageBox::warning(this, tr("Error"), tr("Core is uninitialized."));
    return;
  }
  {
    const Core::CPUThreadGuard guard{m_system};
    m_table_model->OnBranchNotOverwritten(guard);
    AutoSave(guard);
  }
  UpdateStatus();
}

void BranchWatchDialog::OnWipeRecentHits()
{
  m_table_model->OnWipeRecentHits();
}

void BranchWatchDialog::OnWipeInspection()
{
  m_table_model->OnWipeInspection();
}

void BranchWatchDialog::OnTimeout()
{
  Update();
}

void BranchWatchDialog::OnEmulationStateChanged(Core::State new_state)
{
  if (!isVisible())
    return;

  if (TimerCondition(m_branch_watch, new_state))
    m_timer->start(BRANCH_WATCH_TOOL_TIMER_DELAY_MS);
  else if (m_timer->isActive())
    m_timer->stop();
  Update();
}

void BranchWatchDialog::OnHelp()
{
  ModalMessageBox::information(
      this, tr("Branch Watch Tool Help (1/4)"),
      tr("Branch Watch is a code-searching tool that can isolate branches tracked by the emulated "
         "CPU by testing candidate branches with simple criteria. If you are familiar with Cheat "
         "Engine's Ultimap, Branch Watch is similar to that.\n\n"
         "Press the \"Start Branch Watch\" button to activate Branch Watch. Branch Watch persists "
         "across emulation sessions, and a snapshot of your progress can be saved to and loaded "
         "from the User Directory to persist after Dolphin Emulator is closed. \"Save As...\" and "
         "\"Load From...\" actions are also available, and auto-saving can be enabled to save a "
         "snapshot at every step of a search. The \"Pause Branch Watch\" button will halt Branch "
         "Watch from tracking further branch hits until it is told to resume. Press the \"Clear "
         "Branch Watch\" button to clear all candidates and return to the blacklist phase."));
  ModalMessageBox::information(
      this, tr("Branch Watch Tool Help (2/4)"),
      tr("Branch Watch starts in the blacklist phase, meaning no candidates have been chosen yet, "
         "but candidates found so far can be excluded from the candidacy by pressing the \"Code "
         "Path Not Taken\", \"Branch Was Overwritten\", and \"Branch Not Overwritten\" buttons. "
         "Once the \"Code Path Was Taken\" button is pressed for the first time, Branch Watch will "
         "switch to the reduction phase, and the table will populate with all eligible "
         "candidates."));
  ModalMessageBox::information(
      this, tr("Branch Watch Tool Help (3/4)"),
      tr("Once in the reduction phase, it is time to start narrowing down the candidates shown in "
         "the table. Further reduce the candidates by checking whether a code path was or was not "
         "taken since the last time it was checked. It is also possible to reduce the candidates "
         "by determining whether a branch instruction has or has not been overwritten since it was "
         "first hit. Filter the candidates by branch kind, branch condition, origin or destination "
         "address, and origin or destination symbol name.\n\n"
         "After enough passes and experimentation, you may be able to find function calls and "
         "conditional code paths that are only taken when an action is performed in the emulated "
         "software."));
  ModalMessageBox::information(
      this, tr("Branch Watch Tool Help (4/4)"),
      tr("Rows in the table can be left-clicked on the origin, destination, and symbol columns to "
         "view the associated address in Code View. Right-clicking the selected row(s) will bring "
         "up a context menu.\n\n"
         "If the origin column of a row selection is right-clicked, an action to replace the "
         "branch instruction at the origin(s) with a NOP instruction (No Operation), and an action "
         "to copy the address(es) to the clipboard will be available.\n\n"
         "If the destination column of a row selection is right-clicked, an action to replace the "
         "instruction at the destination(s) with a BLR instruction (Branch to Link Register) will "
         "be available, but only if the branch instruction at every origin saves the link "
         "register, and an action to copy the address(es) to the clipboard will be available.\n\n"
         "If the origin / destination symbol column of a row selection is right-clicked, an action "
         "to replace the instruction(s) at the start of the symbol with a BLR instruction will be "
         "available, but only if every origin / destination symbol is found.\n\n"
         "All context menus have the action to delete the selected row(s) from the candidates."));
}

void BranchWatchDialog::OnToggleAutoSave(bool checked)
{
  if (!checked)
    return;

  const QString filepath = DolphinFileDialog::getSaveFileName(
      this, tr("Select Branch Watch snapshot auto-save file (for user folder location, cancel)"),
      QString::fromStdString(File::GetUserPath(D_DUMPDEBUG_BRANCHWATCH_IDX)),
      tr("Text file (*.txt);;All Files (*)"));
  if (filepath.isEmpty())
    m_autosave_filepath = std::nullopt;
  else
    m_autosave_filepath = filepath.toStdString();
}

void BranchWatchDialog::OnHideShowControls(bool checked)
{
  if (checked)
    m_control_toolbar->hide();
  else
    m_control_toolbar->show();
}

void BranchWatchDialog::OnToggleIgnoreApploader(bool checked)
{
  m_system.SetIsBranchWatchIgnoreApploader(checked);
}

void BranchWatchDialog::OnTableClicked(const QModelIndex& index)
{
  const QVariant v = m_table_proxy->data(index, UserRole::ClickRole);
  switch (index.column())
  {
  case Column::OriginSymbol:
  case Column::DestinSymbol:
    if (!v.isValid())
      return;
    [[fallthrough]];
  case Column::Origin:
  case Column::Destination:
    m_code_widget->SetAddress(v.value<u32>(), CodeViewWidget::SetAddressUpdate::WithDetailedUpdate);
    return;
  }
}

void BranchWatchDialog::OnTableContextMenu(const QPoint& pos)
{
  const QModelIndex index = m_table_view->indexAt(pos);
  if (!index.isValid())
    return;
  QModelIndexList index_list = m_table_view->selectionModel()->selectedRows(index.column());

  QMenu* const menu = new QMenu;
  menu->addAction(tr("&Delete"), [this, index_list]() { OnTableDelete(std::move(index_list)); });
  switch (index.column())
  {
  case Column::Origin:
  {
    QAction* const action = menu->addAction(tr("Insert &NOP"));
    if (Core::GetState() != Core::State::Uninitialized)
      connect(action, &QAction::triggered,
              [this, index_list]() { OnTableSetNOP(std::move(index_list)); });
    else
      action->setEnabled(false);
    menu->addAction(tr("&Copy Address"), [this, index_list = std::move(index_list)]() {
      OnTableCopyAddress(std::move(index_list));
    });
    break;
  }
  case Column::Destination:
  {
    QAction* const action = menu->addAction(tr("Insert &BLR"));
    const bool enable_action =
        Core::GetState() != Core::State::Uninitialized &&
        std::all_of(index_list.begin(), index_list.end(), [this](const QModelIndex& index) {
          const QModelIndex sibling = index.siblingAtColumn(Column::Instruction);
          return BranchSavesLR(m_table_proxy->data(sibling, UserRole::ClickRole).value<u32>());
        });
    if (enable_action)
      connect(action, &QAction::triggered,
              [this, index_list]() { OnTableSetBLR(std::move(index_list)); });
    else
      action->setEnabled(false);
    menu->addAction(tr("&Copy Address"), [this, index_list = std::move(index_list)]() {
      OnTableCopyAddress(std::move(index_list));
    });
    break;
  }
  case Column::OriginSymbol:
  case Column::DestinSymbol:
  {
    QAction* const action = menu->addAction(tr("Insert &BLR at start"));
    const bool enable_action =
        Core::GetState() != Core::State::Uninitialized &&
        std::all_of(index_list.begin(), index_list.end(), [this](const QModelIndex& index) {
          return m_table_proxy->data(index, UserRole::ClickRole).isValid();
        });
    if (enable_action)
      connect(action, &QAction::triggered, [this, index_list = std::move(index_list)]() {
        OnTableSetBLR(std::move(index_list));
      });
    else
      action->setEnabled(false);
    break;
  }
  }
  menu->exec(m_table_view->viewport()->mapToGlobal(pos));
}

void BranchWatchDialog::OnTableHeaderContextMenu(const QPoint& pos)
{
  m_mnu_column_visibility->exec(m_table_view->horizontalHeader()->mapToGlobal(pos));
}

void BranchWatchDialog::OnTableDelete(QModelIndexList index_list)
{
  m_table_proxy->OnDelete(std::move(index_list));
  UpdateStatus();
}

void BranchWatchDialog::OnTableDeleteKeypress()
{
  OnTableDelete(m_table_view->selectionModel()->selectedRows());
}

void BranchWatchDialog::OnTableSetBLR(QModelIndexList index_list)
{
  for (const QModelIndex& index : index_list)
  {
    m_system.GetPowerPC().GetDebugInterface().SetPatch(
        Core::CPUThreadGuard{m_system},
        m_table_proxy->data(index, UserRole::ClickRole).value<u32>(), 0x4e800020);
    m_table_proxy->SetInspected(index);
  }
  // TODO: This is not ideal. What I need is a signal for when memory has been changed by the GUI,
  // but I cannot find one. UpdateDisasmDialog comes close, but does too much in one signal. For
  // example, CodeViewWidget will scroll to the current PC when UpdateDisasmDialog is signaled. This
  // seems like a pervasive issue. For example, modifying an instruction in the CodeViewWidget will
  // not reflect in the MemoryViewWidget, and vice versa. Neither of these widgets changing memory
  // will reflect in the JITWidget, either. At the very least, we can make sure the CodeWidget
  // is updated in an acceptable way.
  m_code_widget->Update();
}

void BranchWatchDialog::OnTableSetNOP(QModelIndexList index_list)
{
  for (const QModelIndex& index : index_list)
  {
    m_system.GetPowerPC().GetDebugInterface().SetPatch(
        Core::CPUThreadGuard{m_system},
        m_table_proxy->data(index, UserRole::ClickRole).value<u32>(), 0x60000000);
    m_table_proxy->SetInspected(index);
  }
  // Same issue as OnSetBLR.
  m_code_widget->Update();
}

void BranchWatchDialog::OnTableCopyAddress(QModelIndexList index_list)
{
  auto iter = index_list.begin();
  if (iter == index_list.end())
    return;

  QString text;
  text.reserve(index_list.size() * 9 - 1);
  while (true)
  {
    text.append(QString::number(m_table_proxy->data(*iter, UserRole::ClickRole).value<u32>(), 16));
    if (++iter == index_list.end())
      break;
    text.append(QChar::fromLatin1('\n'));
  }
  QApplication::clipboard()->setText(text);
}

void BranchWatchDialog::SaveSettings()
{
  auto& settings = Settings::GetQSettings();
  settings.setValue(QStringLiteral("branchwatchdialog/geometry"), saveGeometry());
  settings.setValue(QStringLiteral("branchwatchdialog/tableheader/state"),
                    m_table_view->horizontalHeader()->saveState());
}

void BranchWatchDialog::Update()
{
  if (m_branch_watch.GetRecordingPhase() == Core::BranchWatch::Phase::Blacklist)
    UpdateStatus();
  m_table_model->UpdateHits();
}

void BranchWatchDialog::UpdateSymbols()
{
  m_table_model->UpdateSymbols();
}

void BranchWatchDialog::UpdateStatus()
{
  switch (m_branch_watch.GetRecordingPhase())
  {
  case Core::BranchWatch::Phase::Blacklist:
  {
    const std::size_t candidate_size = m_branch_watch.GetCollectionSize();
    const std::size_t blacklist_size = m_branch_watch.GetBlacklistSize();
    if (blacklist_size == 0)
    {
      m_status_bar->showMessage(tr("Candidates: %1").arg(candidate_size));
      return;
    }
    m_status_bar->showMessage(tr("Candidates: %1 | Excluded: %2 | Remaining: %3")
                                  .arg(candidate_size)
                                  .arg(blacklist_size)
                                  .arg(candidate_size - blacklist_size));
    return;
  }
  case Core::BranchWatch::Phase::Reduction:
  {
    const std::size_t candidate_size = m_branch_watch.GetSelection().size();
    if (candidate_size == 0)
    {
      m_status_bar->showMessage(tr("Zero candidates remaining."));
      return;
    }
    const std::size_t remaining_size = m_table_proxy->rowCount();
    m_status_bar->showMessage(tr("Candidates: %1 | Filtered: %2 | Remaining: %3")
                                  .arg(candidate_size)
                                  .arg(candidate_size - remaining_size)
                                  .arg(remaining_size));
    return;
  }
  }
}

void BranchWatchDialog::Save(const Core::CPUThreadGuard& guard, const std::string& filepath)
{
  File::IOFile file(filepath, "w");
  if (!file.IsOpen())
  {
    ModalMessageBox::warning(
        this, tr("Error"),
        tr("Failed to save Branch Watch snapshot \"%1\"").arg(QString::fromStdString(filepath)));
    return;
  }

  m_table_model->Save(guard, file.GetHandle());
}

void BranchWatchDialog::Load(const Core::CPUThreadGuard& guard, const std::string& filepath)
{
  File::IOFile file(filepath, "r");
  if (!file.IsOpen())
  {
    ModalMessageBox::warning(
        this, tr("Error"),
        tr("Failed to open Branch Watch snapshot \"%1\"").arg(QString::fromStdString(filepath)));
    return;
  }

  m_table_model->Load(guard, file.GetHandle());
  m_btn_wipe_recent_hits->setEnabled(m_branch_watch.GetRecordingPhase() ==
                                     Core::BranchWatch::Phase::Reduction);
}

void BranchWatchDialog::AutoSave(const Core::CPUThreadGuard& guard)
{
  if (!m_act_autosave->isChecked() || !m_branch_watch.CanSave())
    return;
  Save(guard, m_autosave_filepath ? m_autosave_filepath.value() : GetSnapshotDefaultFilepath());
}
