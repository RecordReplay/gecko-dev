function reduxDevtoolsContentScript(window){
  (() => { // webpackBootstrap
    /******/ 	"use strict";
    var __webpack_exports__ = {};
    
    ;// CONCATENATED MODULE: ./src/pageScript/api/generateInstanceId.ts
    let id = 0;
    function generateId(instanceId) {
      return instanceId || ++id;
    }
    ;// CONCATENATED MODULE: ./src/pageScript/api/filters.ts
    const FilterState = {
      DO_NOT_FILTER: 'DO_NOT_FILTER',
      DENYLIST_SPECIFIC: 'DENYLIST_SPECIFIC',
      ALLOWLIST_SPECIFIC: 'ALLOWLIST_SPECIFIC'
    };
    const noFiltersApplied = localFilter => !localFilter && (!window.devToolsOptions || !window.devToolsOptions.filter || window.devToolsOptions.filter === FilterState.DO_NOT_FILTER);
    function isFiltered(action, localFilter) {
      if (noFiltersApplied(localFilter) || typeof action !== 'string' && typeof action.type.match !== 'function') {
        return false;
      }
      const {
        allowlist,
        denylist
      } = localFilter || window.devToolsOptions || {};
      const actionType = action.type || action;
      return allowlist && !actionType.match(allowlist) || denylist && actionType.match(denylist);
    }
    ;// CONCATENATED MODULE: ./src/pageScript/api/index.ts
    
    
    const listeners = {};
    const source = '@devtools-page';
    function isArray(arg) {
      return Array.isArray(arg);
    }
    function getLocalFilter(config) {
      const denylist = config.actionsDenylist ?? config.actionsBlacklist;
      const allowlist = config.actionsAllowlist ?? config.actionsWhitelist;
      if (denylist || allowlist) {
        return {
          allowlist: isArray(allowlist) ? allowlist.join('|') : allowlist,
          denylist: isArray(denylist) ? denylist.join('|') : denylist
        };
      }
      return undefined;
    }
    let latestDispatchedActions = {};
    function saveReplayAnnotation(action, state, connectionType, extractedConfig, config) {
      const {
        instanceId
      } = extractedConfig;
      window.__RECORD_REPLAY_ANNOTATION_HOOK__('redux-devtools-setup', JSON.stringify({
        type: 'action',
        actionType: action.type,
        connectionType,
        instanceId
      }));
      latestDispatchedActions[instanceId] = {
        action,
        state,
        extractedConfig,
        config
      };
    }
    function sendMessage(action, state, preConfig = {}, instanceId, name) {
      if (!action || !action.type) {
        action = {
          type: 'update'
        };
      } else if (typeof action === 'string') {
        action = {
          type: action
        };
      }
      const [config, extractedExtensionConfig] = extractExtensionConfig(preConfig);
      instanceId = instanceId ?? extractedExtensionConfig.instanceId;
      saveReplayAnnotation(action, state, 'generic', extractedExtensionConfig, config);
    }
    function extractExtensionConfig(preConfig) {
      const config = preConfig || {};
      const instanceId = generateId(config.instanceId);
      if (!config.instanceId) config.instanceId = instanceId;
      if (!config.name) {
        config.name = document.title && instanceId === 1 ? document.title : `Instance ${instanceId}`;
      }
      const localFilter = getLocalFilter(config);
      let {
        stateSanitizer,
        actionSanitizer,
        predicate
      } = config;
      const extractedExtensionConfig = {
        instanceId: instanceId,
        stateSanitizer,
        actionSanitizer,
        predicate,
        localFilter,
        isFiltered: isFiltered
      };
      return [config, extractedExtensionConfig];
    }
    function connect(preConfig) {
      const [config, extractedExtensionConfig] = extractExtensionConfig(preConfig);
      const {
        instanceId
      } = extractedExtensionConfig;
      const subscribe = listener => {
        if (!listener) return undefined;
        return function unsubscribe() {};
      };
      const unsubscribe = () => {
        delete listeners[instanceId];
      };
      const send = (action, state) => {
        if (!action) {
          return;
        }
        let amendedAction = action;
        if (typeof action === 'string') {
          amendedAction = {
            type: action
          };
        }
        saveReplayAnnotation(amendedAction, state, 'generic', extractedExtensionConfig, config);
        return;
      };
      const init = (state, liftedData) => {
        window.__RECORD_REPLAY_ANNOTATION_HOOK__('redux-devtools-setup', JSON.stringify({
          type: 'init',
          connectionType: 'generic',
          instanceId
        }));
      };
      const error = payload => {};
      return {
        init,
        subscribe,
        unsubscribe,
        send,
        error
      };
    }
    ;// CONCATENATED MODULE: ./src/pageScript/index.ts
    
    
    let stores = {};
    function __REDUX_DEVTOOLS_EXTENSION__(preConfig = {}) {
      // if (typeof config !== 'object') config = {};
      if (!window.devToolsOptions) window.devToolsOptions = {};
      let store;
      const [config, extractedExtensionConfig] = extractExtensionConfig(preConfig);
      const {
        instanceId
      } = extractedExtensionConfig;
      function init() {
        window.__RECORD_REPLAY_ANNOTATION_HOOK__('redux-devtools-setup', JSON.stringify({
          type: 'init',
          connectionType: 'redux',
          instanceId
        }));
      }
      const enhance = () => next => {
        return (reducer_, initialState_) => {
          const originalStore = next(reducer_, initialState_);
          const newStore = {
            ...originalStore,
            dispatch: action => {
              const result = originalStore.dispatch(action);
              saveReplayAnnotation(action, originalStore.getState(), 'redux', extractedExtensionConfig, config);
              return result;
            }
          };
    
          // @ts-ignore
          store = stores[instanceId] = newStore;
          init();
          return store;
        };
      };
      return enhance();
    }
    // noinspection JSAnnotator
    window.__REDUX_DEVTOOLS_EXTENSION__ = __REDUX_DEVTOOLS_EXTENSION__;
    window.__REDUX_DEVTOOLS_EXTENSION__.open = () => {};
    window.__REDUX_DEVTOOLS_EXTENSION__.notifyErrors = () => {};
    window.__REDUX_DEVTOOLS_EXTENSION__.send = sendMessage;
    window.__REDUX_DEVTOOLS_EXTENSION__.listen = () => {};
    window.__REDUX_DEVTOOLS_EXTENSION__.connect = connect;
    window.__REDUX_DEVTOOLS_EXTENSION__.disconnect = () => {};
    const extensionCompose = config => (...funcs) => {
      // @ts-ignore FIXME
      return (...args) => {
        const instanceId = generateId(config.instanceId);
        return [...funcs].reduceRight(
        // @ts-ignore FIXME
        (composed, f) => f(composed), __REDUX_DEVTOOLS_EXTENSION__({
          ...config,
          instanceId
        })(...args));
      };
    };
    function reduxDevtoolsExtensionCompose(...funcs) {
      if (funcs.length === 0) {
        return __REDUX_DEVTOOLS_EXTENSION__();
      }
      if (funcs.length === 1 && typeof funcs[0] === 'object') {
        return extensionCompose(funcs[0]);
      }
      return extensionCompose({})(...funcs);
    }
    window.__REDUX_DEVTOOLS_EXTENSION_COMPOSE__ = reduxDevtoolsExtensionCompose;
    /******/ })()

/******/ }

exports.reduxDevtoolsContentScript = reduxDevtoolsContentScript
//# sourceMappingURL=page.bundle.js.map