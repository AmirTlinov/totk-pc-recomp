"""Keep fixed development cache overrides isolated across shader generations."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from macos.build import BUILD


def main():
    with tempfile.TemporaryDirectory(dir=BUILD, prefix='shader-cache-test-') as tmp:
        work = Path(tmp)
        source = work / 'cache.m'
        source.write_text('''#include "native_shader_cache.h"
#include <assert.h>
int main(void) { @autoreleasepool {
    NSString *a=[@"a" stringByPaddingToLength:64 withString:@"a" startingAtIndex:0];
    NSString *b=[@"b" stringByPaddingToLength:64 withString:@"b" startingAtIndex:0];
    NSString *base=@"/fixed/shaders", *first=native_shader_cache_resolve(base,a,@"26A1",42);
    assert(([first isEqual:[base stringByAppendingFormat:@"/%@-26A1-2a",a]]));
    assert([native_shader_cache_resolve(first,a,@"26A1",42) isEqual:first]);
    assert(![native_shader_cache_resolve(base,b,@"26A1",42) isEqual:first]);
    assert(![native_shader_cache_resolve(base,a,@"26A2",42) isEqual:first]);
    assert(![native_shader_cache_resolve(base,a,@"26A1",43) isEqual:first]);
    assert(!native_shader_cache_resolve(base,@"invalid",@"26A1",42));
    assert(!native_shader_cache_resolve(base,[a stringByReplacingCharactersInRange:NSMakeRange(0,1) withString:@"/"],@"26A1",42));
    assert(!native_shader_cache_resolve(base,a,@"",42));
    puts("{\\"probe\\":\\"shader-cache-generation\\",\\"status\\":\\"pass\\",\\"checks\\":8}");
    return 0;
}}
''')
        executable = work / 'cache'
        subprocess.run(['xcrun', 'clang', '-O2', '-fobjc-arc', '-mmacosx-version-min=26.0',
                        '-I', ROOT / 'runtime/macos', '-framework', 'Foundation', '-framework', 'Metal',
                        source, '-o', executable], check=True)
        subprocess.run([executable], check=True)


if __name__ == '__main__':
    main()
