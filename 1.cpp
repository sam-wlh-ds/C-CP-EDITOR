#include <bits/stdc++.h>
using namespace std;

void dbg_out() { cerr << endl; }
template<typename Head, typename... Tail>
void dbg_out(Head H, Tail... T) {cerr << ' ' << H;dbg_out(T...);}
#define dbg(...) cerr << "(" << #__VA_ARGS__ << "):", dbg_out(__VA_ARGS__)

#define ll long long
#define int ll
#define rep(i, a, b) for(int i = a; i < (b); ++i)
#define all(x) begin(x), end(x)
#define sz(x) (int)(x).size()
typedef pair<int, int> pii;
typedef vector<int> vi;

//#include "debugging.h"

#define mod 1000000007

void printHi(){
	cout << "Hi\n";
}

void solve()
{
    int n;
    cin >> n;

    vi a(n);
    rep(i,0,n){
        cin >> a[i];
    }


}

signed main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    ll test=1;
    cin>>test;

    while(test--)
    {
        solve();
    }
    return 0;
}
