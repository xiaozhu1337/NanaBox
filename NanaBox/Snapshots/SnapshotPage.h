#pragma once

#include "SnapshotPage.g.h"
#include "SnapshotStore.h"
#include "Utils.h"

namespace winrt
{
    using Windows::Foundation::IInspectable;
    using Windows::UI::Xaml::RoutedEventArgs;
}

namespace NanaBox
{
    enum class SnapshotAction { None, Create, Restore, Delete };

    struct SnapshotRequest
    {
        SnapshotAction Action = SnapshotAction::None;
        std::string Id;
        std::string Name;
        std::string Description;
    };
}

namespace winrt::NanaBox::implementation
{
    struct SnapshotPage : SnapshotPageT<SnapshotPage>
    {
        SnapshotPage(HWND WindowHandle = nullptr,
            std::wstring const& ConfigurationPath = L"");
        void InitializeComponent();
        void PageLoaded(winrt::IInspectable const&, winrt::RoutedEventArgs const&);
        void CreateClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&);
        void RestoreClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&);
        void DeleteClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&);
        void PreviousClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&);
        void NextClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&);
        void CloseClick(winrt::IInspectable const&, winrt::RoutedEventArgs const&);
        void SelectionChanged(winrt::IInspectable const&,
            winrt::Windows::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        ::NanaBox::SnapshotRequest Request() const;

    private:
        HWND m_WindowHandle;
        ::NanaBox::SnapshotStore m_Store;
        ::NanaBox::SnapshotRequest m_Request;
        std::size_t m_Page = 0;
        ::NanaBox::SnapshotPageResult m_Result;
        void Refresh();
        void Select(::NanaBox::SnapshotAction Action);
    };
}

namespace winrt::NanaBox::factory_implementation
{
    struct SnapshotPage : SnapshotPageT<SnapshotPage, implementation::SnapshotPage> {};
}
