#pragma once

#include <Plugin.h>
#include <PluginDef.h>

using namespace winrt;
using namespace Windows::ApplicationModel::AppService;
using namespace Windows::Foundation;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;
using namespace Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Media;

class PluginManager {

public:

	PluginManager() {
		PyImport_AppendInittab("myapp", PyInit_myapp);
		Py_Initialize();

		LoadAllPlugins();
	}

	~PluginManager() {
		Py_Finalize();
	}

	static PluginManager& GetInstance() {
		static PluginManager instance;
		return instance;
	}

	winrt::Windows::Foundation::Collections::IObservableVector<winrt::Windows::Foundation::IInspectable> m_plugins{ winrt::single_threaded_observable_vector<winrt::Windows::Foundation::IInspectable>() };

	void PrintPythonError()
	{
		PyObject* ptype, * pvalue, * ptraceback;
		PyErr_Fetch(&ptype, &pvalue, &ptraceback); // Fetch the error

		PyObject* strValue = PyObject_Str(pvalue);
		const char* errorMsg = PyUnicode_AsUTF8(strValue);
		OutputDebugStringA(errorMsg); // Output the error to Debug window

		Py_XDECREF(ptype);
		Py_XDECREF(pvalue);
		Py_XDECREF(ptraceback);
		Py_XDECREF(strValue);
	}


	IAsyncAction LoadAllPlugins() {
		m_plugins.Clear();

		try {
			StorageFolder localFolder = ApplicationData::Current().LocalFolder();
			IStorageItem pluginFolder = co_await localFolder.TryGetItemAsync(L"Plugins");

			// Ensure plugin folder exists
			StorageFolder pluginsFolder = nullptr;
			if (pluginFolder == nullptr) {
				std::wcout << L"Plugins folder not found. Creating it..." << std::endl;
				pluginsFolder = co_await ApplicationData::Current().LocalFolder().CreateFolderAsync(L"Plugins");
			}
			else if (auto folder = pluginFolder.try_as<StorageFolder>()) {
				pluginsFolder = folder;
				std::wcout << L"Plugins folder found: " << pluginsFolder.Path().c_str() << std::endl;
			}
			else {
				throw winrt::hresult_error(E_FAIL, L"'Plugins' exists but is not a folder");
			}

			// Get all folders
			const auto& folders = co_await pluginsFolder.GetFoldersAsync();

			for (const auto& entry : folders) {
				try {
					auto pluginPath = entry.Path();
					const auto& jsonFile = co_await entry.TryGetItemAsync(L"plugin.json");
					if (!jsonFile) {
						std::wcerr << L"plugin.json not found in " << entry.Name().c_str() << std::endl;
						continue;
					}

					auto storageFile = jsonFile.try_as<StorageFile>();

					const auto& fileContent = co_await FileIO::ReadTextAsync(storageFile);
					std::string jsonData = winrt::to_string(fileContent);
					nlohmann::json pluginData = nlohmann::json::parse(jsonData);

					std::string pluginName = pluginData["name"];
					std::string pluginDescription = pluginData["description"];
					std::string pluginVersion = pluginData["version"];
					std::string pluginAuthor = pluginData["author"];
					std::unordered_map<std::string, std::string> pluginActions = pluginData["actions"].get<std::unordered_map<std::string, std::string>>();

					std::string pluginFolderStr = winrt::to_string(pluginPath);
					PyGILState_STATE gstate = PyGILState_Ensure();

					try {
						PyRun_SimpleString(("import sys; sys.path.append(r'" + pluginFolderStr + "')").c_str());

						// Import plugin
						PyObject* pName = PyUnicode_DecodeFSDefault(pluginName.c_str());
						if (!pName) {
							PrintPythonError();
							continue;
						}

						PyObject* pModule = PyImport_Import(pName);
						if (!pModule) {
							PrintPythonError();
							Py_DECREF(pName);
							continue;
						}

						PyObject* pClass = PyObject_GetAttrString(pModule, pluginName.c_str());
						if (pClass && PyCallable_Check(pClass)) {
							PyObject* pInstance = PyObject_CallObject(pClass, nullptr);
							if (pInstance) {
								m_plugins.Append(winrt::make<CPythonIntrop::implementation::Plugin>(to_hstring(pluginName), to_hstring(pluginDescription), to_hstring(pluginVersion), to_hstring(pluginAuthor), pluginActions, pInstance, pluginPath, true));
							}
							else {
								PyErr_Print();
							}
						}
						else {
							PyErr_Print();
						}
						// Cleanup
						Py_XDECREF(pClass);
						Py_DECREF(pModule);
						Py_DECREF(pName);
					}
					catch (...) {
						std::wcerr << L"Error processing plugin: " << entry.Name().c_str() << std::endl;
					}

					PyGILState_Release(gstate);

					// Continue cleanup and handling
				}
				catch (const winrt::hresult_error& ex) {
					OutputDebugString((L"Error processing folder: " + entry.Name() + L" - " + ex.message()).c_str());
				}
			}
		}
		catch (const winrt::hresult_error& ex) {
			OutputDebugString((L"Critical error in LoadAllPlugins: " + ex.message()).c_str());
		}
	}

	struct PluginException : public winrt::hresult_error
	{
		PluginException(winrt::hresult const& Code, winrt::hstring const& Message) : winrt::hresult_error(Code, Message) {}
	};

	IAsyncAction RemovePlugin(CPythonIntrop::Plugin plugin)
	{
		try
		{
			StorageFolder localFolder = co_await StorageFolder::GetFolderFromPathAsync(plugin.PluginFolderPath());
			co_await localFolder.DeleteAsync(StorageDeleteOption::PermanentDelete);
		}
		catch (winrt::hresult_error const& ex)
		{
			HRESULT hr = ex.code();
			winrt::hstring const& hmessage = ex.message();
			winrt::hstring errorMessage{ hstring(L"HRESULT Error 0x") + hr + hstring(L" : ") + hmessage };

			if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
			{
				OutputDebugStringW(L"Plugin folder not found.\n");
				co_return;
			}
			else if (hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION))
			{
				errorMessage = errorMessage + L" (Folder may be in use).";
			}
			else if (hr == E_ACCESSDENIED)
			{
				errorMessage = errorMessage + L" (Access denied).";
			}

			OutputDebugStringW(errorMessage.c_str());
			OutputDebugStringW(L"\n");
			throw PluginException(hr, errorMessage);
		}
		catch (...)
		{
			OutputDebugStringW(L"Unknown Error.\n");
			throw PluginException(hresult(0), L"An unknown error occurred.");
		}
	}

	IAsyncOperation<hstring> GetPluginsFolderPath() {
		try {
			StorageFolder localFolder = ApplicationData::Current().LocalFolder();
			IStorageItem pluginFolder = co_await localFolder.TryGetItemAsync(L"Plugins");

			StorageFolder pluginsFolder = nullptr;
			if (pluginFolder == nullptr) {
				std::wcout << L"Plugins folder not found. Creating it..." << std::endl;
				pluginsFolder = co_await ApplicationData::Current().LocalFolder().CreateFolderAsync(L"Plugins");
			}
			else if (auto folder = pluginFolder.try_as<StorageFolder>()) {
				pluginsFolder = folder;
				std::wcout << L"Plugins folder found: " << pluginsFolder.Path().c_str() << std::endl;
			}
			else {
				throw winrt::hresult_error(E_FAIL, L"'Plugins' exists but is not a folder");
			}

			co_return pluginsFolder.Path();
		}
		catch (winrt::hresult_error const& ex)
		{
			HRESULT hr = ex.code();
			winrt::hstring const& hmessage = ex.message();
			winrt::hstring errorMessage{ hstring(L"HRESULT Error 0x") + hr + hstring(L" : ") + hmessage };

			if (hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
			{
				OutputDebugStringW(L"Plugins folder not found.\n");
				co_return L"Plugins folder not found.\n";
			}
			else if (hr == E_ACCESSDENIED)
			{
				OutputDebugStringW(L"Access denied.\n");
				errorMessage = errorMessage + L" (Access denied).";
			}

			throw PluginException(hr, errorMessage);
		}
		catch (...)
		{
			OutputDebugStringW(L"Unknown Error.\n");
			throw PluginException(hresult(0), L"An unknown error occurred.");
		}
	}

	IAsyncAction BroadcastEvent(const std::string& eventName) {
		PyGILState_STATE gstate = PyGILState_Ensure(); // Acquire GIL

		for (const auto& pluginInspectable : m_plugins) {
			auto const& plugin = pluginInspectable.as<CPythonIntrop::implementation::Plugin>().get();

			if (plugin && !plugin->isPluginEnabled()) {
				continue;
			}

			auto const& actions = plugin->PluginActions();
			auto it = actions.find(eventName);

			if (it != actions.end()) {
				std::string methodName = it->second;
				if (!methodName.empty()) {
					PyObject* result = PyObject_CallMethod(plugin->PluginInstance(), methodName.c_str(), nullptr);
					if (result == nullptr) {
						PyErr_Print(); // Print any Python errors
					}
					else {
						Py_DECREF(result); // Clean up the result if call was successful
					}
				}
			}
			else {
				std::cerr << "Event " << eventName << " not found in plugin " << to_string(plugin->PluginName()) << std::endl;
			}
		}

		PyGILState_Release(gstate); // Release GIL

		co_return;
	}
};