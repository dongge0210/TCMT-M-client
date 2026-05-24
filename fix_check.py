import xml.etree.ElementTree as ET

# MainWindow.axaml 中 ListBox 的绑定结构
# <ListBox ItemsSource="{Binding Pages}" SelectedItem="{Binding CurrentPage}">
#   <ListBox.ItemTemplate>
#     <DataTemplate>
#       <TextBlock Text="{Binding Title}" />
#     </DataTemplate>
#   </ListBox.ItemTemplate>
# </ListBox>

print("Checking ViewModelBase Title property...")
# ViewModelBase.cs
# [ObservableProperty]
# private string _title = "";
