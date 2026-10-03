#include "pch.h"
#include "SnapshotPage.h"
#if __has_include("SnapshotPage.g.cpp")
#include "SnapshotPage.g.cpp"
#endif
#include "SnapshotFiles.h"

namespace winrt::NanaBox::implementation
{
    SnapshotPage::SnapshotPage(HWND WindowHandle,
        std::wstring const& ConfigurationPath) :
        m_WindowHandle(WindowHandle), m_Store(ConfigurationPath)
    {
        ::SetWindowTextW(WindowHandle,
            Mile::WinRT::GetLocalizedString(L"Snapshots/Title/Text").c_str());
    }

    void SnapshotPage::InitializeComponent()
    {
        SnapshotPageT::InitializeComponent();
    }

    void SnapshotPage::PageLoaded(winrt::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        this->Refresh();
    }

    void SnapshotPage::Refresh()
    {
        try
        {
            this->m_Result = this->m_Store.List(this->m_Page, 20);
            this->SnapshotList().Items().Clear();
            for (auto const& Item : this->m_Result.Items)
            {
                std::string Text = Item.Name + "  |  " + Item.CreatedUtc +
                    "\n" + Item.Id + "\n" + Item.Description;
                if (!Item.ParentId.empty())
                    Text += "\n" + winrt::to_string(Mile::WinRT::GetLocalizedString(
                        L"Snapshots/ParentText")) + Item.ParentId;
                if (Item.Current)
                    Text += "\n" + winrt::to_string(Mile::WinRT::GetLocalizedString(
                        L"Snapshots/CurrentText"));
                winrt::Windows::UI::Xaml::Controls::TextBlock Label;
                Label.Text(winrt::to_hstring(Text));
                Label.TextWrapping(winrt::Windows::UI::Xaml::TextWrapping::Wrap);
                this->SnapshotList().Items().Append(Label);
            }
            this->PreviousButton().IsEnabled(this->m_Page > 0);
            this->NextButton().IsEnabled((this->m_Page + 1) * 20 < this->m_Result.Total);
            this->PageText().Text(winrt::to_hstring(std::to_string(this->m_Page + 1) +
                " / " + std::to_string((std::max)(std::size_t(1), (this->m_Result.Total + 19) / 20))));
        }
        catch (...)
        {
            auto Error = Mile::WinRT::ToHResultError();
            ::NanaBox::SnapshotFiles::Log("list", Error);
            this->StatusText().Text(Error.message());
        }
    }

    void SnapshotPage::SelectionChanged(winrt::IInspectable const&,
        winrt::Windows::UI::Xaml::Controls::SelectionChangedEventArgs const&)
    {
        bool Selected = this->SnapshotList().SelectedIndex() >= 0;
        this->RestoreButton().IsEnabled(Selected);
        this->DeleteButton().IsEnabled(Selected);
        this->ConfirmBox().IsChecked(false);
    }

    void SnapshotPage::CreateClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        auto Name = winrt::to_string(this->NameBox().Text());
        if (Name.empty() || Name.size() > 240)
        {
            this->StatusText().Text(Mile::WinRT::GetLocalizedString(L"Snapshots/NameRequired"));
            return;
        }
        this->m_Request = { ::NanaBox::SnapshotAction::Create, "", Name,
            winrt::to_string(this->DescriptionBox().Text()) };
        ::PostMessageW(this->m_WindowHandle, WM_CLOSE, 0, 0);
    }

    void SnapshotPage::Select(::NanaBox::SnapshotAction Action)
    {
        auto Index = this->SnapshotList().SelectedIndex();
        if (Index < 0 || static_cast<std::size_t>(Index) >= this->m_Result.Items.size()) return;
        if (!this->ConfirmBox().IsChecked().GetBoolean())
        {
            this->StatusText().Text(Mile::WinRT::GetLocalizedString(L"Snapshots/ConfirmationRequired"));
            return;
        }
        this->m_Request.Action = Action;
        this->m_Request.Id = this->m_Result.Items.at(Index).Id;
        ::PostMessageW(this->m_WindowHandle, WM_CLOSE, 0, 0);
    }

    void SnapshotPage::RestoreClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        this->Select(::NanaBox::SnapshotAction::Restore);
    }

    void SnapshotPage::DeleteClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        this->Select(::NanaBox::SnapshotAction::Delete);
    }

    void SnapshotPage::PreviousClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        if (this->m_Page) --this->m_Page;
        this->Refresh();
    }

    void SnapshotPage::NextClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        if ((this->m_Page + 1) * 20 < this->m_Result.Total) ++this->m_Page;
        this->Refresh();
    }

    void SnapshotPage::CloseClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&)
    {
        ::PostMessageW(this->m_WindowHandle, WM_CLOSE, 0, 0);
    }

    ::NanaBox::SnapshotRequest SnapshotPage::Request() const
    {
        return this->m_Request;
    }
}
