import { useCallback } from 'react';
import { useDeepCompareEffect, useFirstMountState, useMountedState } from 'react-use';

// run callback when component is mounted only
export const useWhenMounted = () => {
  const isMounted = useMountedState();

  return useCallback(
    (callback: Function): void => {
      if (isMounted()) callback();
    },
    [isMounted]
  );
};

// same as original useEffect() but:
// - ignore the first forced invocation on mount, i.e. run effect on dependencies update only
// - compare deps by content instead of by reference
export const useDeepCompareUpdateEffect: typeof useDeepCompareEffect = (effect, deps) => {
  const isFirstRender = useFirstMountState();

  useDeepCompareEffect(() => {
    if (!isFirstRender) {
      return effect();
    }
  }, deps);
};
